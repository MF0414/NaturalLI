#include <cstring>
#include <sstream>
#include <thread>

#include "SynSearch.h"
#include "Utils.h"

using namespace std;

// ----------------------------------------------
// UTILITIES
// ----------------------------------------------
inline uint64_t mix( const uint64_t u ) {
  uint64_t v = u * 3935559000370003845l + 2691343689449507681l;
  v ^= v >> 21;
  v ^= v << 37;
  v ^= v >>  4;
  v *= 4768777513237032717l;
  v ^= v << 20;
  v ^= v >> 41;
  v ^= v <<  5;
  return v;
}
  
inline uint64_t hashEdge(dependency_edge edge) {
  // Collapse relations which are 'equivalent'
  dep_label  originalRel = edge.relation;
  if (edge.relation == DEP_NEG) { 
    edge.relation = DEP_DET;
  }
  if (edge.relation == DEP_OP) {
    return 0x0;
  }
  // Hash edge
#if TWO_PASS_HASH!=0
  return fnv_64a_buf(&edge, sizeof(dependency_edge), FNV1_64_INIT);
#else
  uint64_t edgeHash;
  memcpy(&edgeHash, &edge, sizeof(uint64_t));
  return mix(edgeHash);
#endif
  // Revert relation
  edge.relation = originalRel;
}


static const uint8_t zero [8] = {0,0,0,0,0,0,0,0};

inline uint64_t hashQuantifiers(const quantifier_monotonicity* quants) {
  uint64_t hash = 0;
  for (uint8_t i = 0; i < MAX_QUANTIFIER_COUNT; ++i) {
    const quantifier_monotonicity& quant = quants[i];
    // If there is no quantifier, don't start hashing it.
    if (memcmp(&quant, zero, sizeof(quantifier_monotonicity)) == 0) {
      continue;
    }
    // If the quantifier is existential, don't hash it.
    if (quant.subj_mono == MONOTONE_UP &&
        (quant.subj_type == QUANTIFIER_TYPE_ADDITIVE ||
         quant.subj_type == QUANTIFIER_TYPE_BOTH) &&
        (quant.obj_mono == MONOTONE_FLAT ||
         (quant.obj_mono == MONOTONE_UP &&
          (quant.obj_type == QUANTIFIER_TYPE_ADDITIVE ||
           quant.obj_type == QUANTIFIER_TYPE_BOTH))) ) {
      // This is an existential quantifier that we can skip.
      continue;
    }
#if TWO_PASS_HASH!=0
    hash ^= fnv_64a_buf((void*) &(quant), sizeof(quantifier_monotonicity), FNV1_64_INIT);
#else
    uint64_t quantHash = 0x0;
    memcpy(&quantHash, &(quant), sizeof(quantifier_monotonicity));
    hash ^= mix(quantHash);
#endif
  }
  return hash;
}

// ----------------------------------------------
// PATH ELEMENT (SEARCH NODE)
// ----------------------------------------------
inline syn_path_data mkSearchNodeData(
    const uint64_t&    factHash,
    const uint8_t&     index,
    const bool&        truth,
    const uint32_t&    deleteMask,
    const tagged_word& currentToken,
    const ::word&      governor,
    const uint64_t& backpointer,
    const bool& allQuantifiersSeen) {
  syn_path_data dat;
  dat.factHash = factHash;
  dat.index = index;
  dat.truth = truth;
  dat.deleteMask = deleteMask;
  dat.currentWord = currentToken.word;
  dat.currentSense = currentToken.sense;
  dat.governor = governor;
  dat.backpointer = backpointer;
  dat.allQuantifiersSeen = allQuantifiersSeen;
  return dat;
}

inline syn_path_data mkSearchNodeData(
    const uint64_t&    factHash,
    const uint8_t&     index,
    const bool&        truth,
    const uint32_t&    deleteMask,
    const ::word&      currentWord,
    const uint8_t&     currentSense,
    const ::word&      governor,
    const uint64_t& backpointer,
    const bool& allQuantifiersSeen) {
  syn_path_data dat;
  dat.factHash = factHash;
  dat.index = index;
  dat.truth = truth;
  dat.deleteMask = deleteMask;
  dat.currentWord = currentWord;
  dat.currentSense = currentSense;
  dat.governor = governor;
  dat.backpointer = backpointer;
  dat.allQuantifiersSeen = allQuantifiersSeen;
  return dat;
}

SearchNode::SearchNode()
    : data(mkSearchNodeData(42l, 255, false, 42, getTaggedWord(0, 0, 0), TREE_ROOT_WORD, 0, false)) { 
  // IMPORTANT: this lets us hash all the quantifiers at once, if we want to.
  memset(this->quantifierMonotonicities, 0, MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}

SearchNode::SearchNode(const SearchNode& from)
    : data(from.data) {
  memcpy(this->quantifierMonotonicities, from.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}
  
SearchNode::SearchNode(const Tree& init)
    : data(mkSearchNodeData(init.hash(), init.root(), true, 
                         0x0, init.token(init.root()), TREE_ROOT_WORD, 0, false)) {
  memcpy(this->quantifierMonotonicities, init.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}
 
SearchNode::SearchNode(const Tree& init, const bool& assumedInitialTruth)
    : data(mkSearchNodeData(init.hash(), init.root(), assumedInitialTruth, 
                         0x0, init.token(init.root()), TREE_ROOT_WORD, 0, false)) {
  memcpy(this->quantifierMonotonicities, init.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}

SearchNode::SearchNode(const Tree& init, const bool& assumedInitialTruth,
                       const uint8_t& index)
    : data(mkSearchNodeData(init.hash(), index, assumedInitialTruth, 
                         0x0, init.token(index), init.word(init.governor(index)), 0, false)) {
  memcpy(this->quantifierMonotonicities, init.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}
 
SearchNode::SearchNode(const Tree& init, const uint8_t& index)
    : data(mkSearchNodeData(init.hash(), index, true, 
                         0x0, init.token(index), init.word(init.governor(index)), 0, false)) {
  memcpy(this->quantifierMonotonicities, init.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}
  
//
// SearchNode() ''mutate constructor
//
SearchNode::SearchNode(const SearchNode& from, const uint64_t& newHash,
                 const tagged_word& newToken,
                 const bool& newTruthValue,
                 const uint32_t& backpointer)
    : data(mkSearchNodeData(newHash, from.data.index, newTruthValue,
                         from.data.deleteMask, newToken,
                         from.data.governor, backpointer, from.data.allQuantifiersSeen)) {
  memcpy(this->quantifierMonotonicities, from.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}

//
// SearchNode() ''delete constructor
//
SearchNode::SearchNode(const SearchNode& from, const uint64_t& newHash,
          const bool& newTruthValue,
          const uint32_t& addedDeletions, const uint32_t& backpointer)
    : data(mkSearchNodeData(newHash, from.data.index, newTruthValue,
                         addedDeletions | from.data.deleteMask, 
                         from.data.currentWord, from.data.currentSense,
                         from.data.governor, backpointer, from.data.allQuantifiersSeen)) { 
  memcpy(this->quantifierMonotonicities, from.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}

//
// SearchNode() ''move index constructor
//
SearchNode::SearchNode(const SearchNode& from, const Tree& tree,
                 const uint8_t& newIndex, const uint32_t& backpointer)
    : data(mkSearchNodeData(from.data.factHash, newIndex, from.data.truth, 
                         from.data.deleteMask, tree.token(newIndex), 
                         tree.token(tree.governor(newIndex)).word, backpointer,
                         from.data.allQuantifiersSeen)) { 
  memcpy(this->quantifierMonotonicities, from.quantifierMonotonicities,
    MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
}
  
//
// SearchNode::mutateQuantifier
//
void SearchNode::mutateQuantifier(
      const uint8_t& quantifierIndex,
      const monotonicity& subjMono,
      const quantifier_type& subjType,
      const monotonicity& objMono,
      const quantifier_type& objType) {
  this->data.factHash ^= hashQuantifiers(this->quantifierMonotonicities);
  this->quantifierMonotonicities[quantifierIndex].subj_mono = subjMono;
  this->quantifierMonotonicities[quantifierIndex].obj_mono = objMono;
  this->quantifierMonotonicities[quantifierIndex].subj_type = subjType;
  this->quantifierMonotonicities[quantifierIndex].obj_type = objType;
  this->data.factHash ^= hashQuantifiers(this->quantifierMonotonicities);
}

//
// SearchNode::deletion
//
SearchNode SearchNode::deletion(const uint32_t& myIndexInHistory,
                                const bool& newTruthValue,
                                const Tree& tree,
                                const uint8_t& dependentIndex) const {
  // Compute deletion
  uint32_t deletionMask = tree.createDeleteMask(dependentIndex);
  uint64_t newHash = tree.updateHashFromDeletions(
      this->factHash(), dependentIndex, tree.token(dependentIndex).word,
      this->word(), deletionMask);
  // Create child node
  SearchNode rtn(*this, newHash, newTruthValue, deletionMask, myIndexInHistory);
  // Handle quantifier deletion
  int8_t quantifierI = tree.quantifierIndex(dependentIndex);
  if (quantifierI >= 0) {
    newHash ^= hashQuantifiers(rtn.quantifierMonotonicities);
    rtn.quantifierMonotonicities[quantifierI].clear();
    newHash ^= hashQuantifiers(rtn.quantifierMonotonicities);
  }
  return rtn;
}

// ----------------------------------------------
// DEPENDENCY TREE
// ----------------------------------------------

//
// Tree::Tree(conll)
//
uint8_t computeLength(const string& conll) {
  uint8_t length = 0;
  stringstream ss(conll);
  string item;
  while (getline(ss, item, '\n')) {
    if (item.length() > 0) {
      length += 1;
    }
  }
  return length;
}

            
void stringToMonotonicity(string field, monotonicity* mono, quantifier_type* type) {
  if (field == "monotone") {
    *mono = MONOTONE_UP; *type = QUANTIFIER_TYPE_NONE;
  } else if (field == "additive") {
    *mono = MONOTONE_UP; *type = QUANTIFIER_TYPE_ADDITIVE;
  } else if (field == "multiplicative") {
    *mono = MONOTONE_UP; *type = QUANTIFIER_TYPE_MULTIPLICATIVE;
  } else if (field == "additive-multiplicative") {
    *mono = MONOTONE_UP; *type = QUANTIFIER_TYPE_BOTH;
  } else if (field == "antitone") {
    *mono = MONOTONE_DOWN; *type = QUANTIFIER_TYPE_NONE;
  } else if (field == "anti-additive") {
    *mono = MONOTONE_DOWN; *type = QUANTIFIER_TYPE_ADDITIVE;
  } else if (field == "anti-multiplicative") {
    *mono = MONOTONE_DOWN; *type = QUANTIFIER_TYPE_MULTIPLICATIVE;
  } else if (field == "anti-additive-multiplicative") {
    *mono = MONOTONE_DOWN; *type = QUANTIFIER_TYPE_BOTH;
  } else if (field == "nonmonotone") {
    *mono = MONOTONE_FLAT; *type = QUANTIFIER_TYPE_NONE;
  } else {
    fprintf(stderr, "ERROR: Bad monotonicity marker: %s; assuming flat: ", field.c_str());
  }
}

void stringToSpan(string field, uint8_t* begin, uint8_t* end) {
  stringstream spanStream(field);
  string spanElem;
  bool isFirst = true;
  while (getline(spanStream, spanElem, '-')) {
    if (isFirst) {
      *begin = atoi(spanElem.c_str()) - 1;
      isFirst = false;
    } else {
      *end = atoi(spanElem.c_str()) - 1;
    }
  }
}

Tree::Tree(const string& conll) 
      : length(computeLength(conll)),
        numQuantifiers(0) {
  memset(this->quantifierMonotonicities, 0, MAX_QUANTIFIER_COUNT * sizeof(quantifier_monotonicity));
  // Variables
  stringstream lineStream(conll);
  string line;
  uint8_t lineI = 0;
  monotonicity subjMono;
  monotonicity objMono;
  quantifier_type subjType;
  quantifier_type objType;
  quantifier_span span;
  
  // Parse CoNLL
  while (getline(lineStream, line, '\n')) {
    if (line.length() == 0) { continue; }
    bool isQuantifier = false;
    stringstream fieldsStream(line);
    string field;
    uint8_t fieldI = 0;
    while (getline(fieldsStream, field, '\t')) {
      switch (fieldI) {
        case 0:  // Word (as integer)
          data[lineI].word = atoi(field.c_str());
          data[lineI].sense = 0;
          break;
        case 1:  // Governor
          data[lineI].governor = atoi(field.c_str());
          if (data[lineI].governor == 0) {
            data[lineI].governor = TREE_ROOT;
          } else {
            data[lineI].governor -= 1;
          }
          break;
        case 2:  // Label
          data[lineI].relation = indexDependency(field.c_str());
          break;
        case 3:  // Word Sense
          data[lineI].sense = atoi(field.c_str());
          break;
        case 4:  // Subject monotonicity
          if (field[0] != '-') {
            isQuantifier = true;
            stringToMonotonicity(field, &subjMono, &subjType);
          }
          break;
        case 5:  // Subject span
          if (field[0] != '-') {
            uint8_t begin, end;
            stringToSpan(field, &begin, &end);
            span.subj_begin = begin; span.subj_end = end;
          }
          break;
        case 6:  // Object monotonicity
          if (field[0] != '-') {
            stringToMonotonicity(field, &objMono, &objType);
          } else {
            objMono = MONOTONE_FLAT;
            objType = QUANTIFIER_TYPE_NONE;
          }
          break;
        case 7:  // Object span
          if (field[0] != '-') {
            uint8_t begin, end;
            stringToSpan(field, &begin, &end);
            span.obj_begin = begin; span.obj_end = end;
          } else {
            span.obj_begin = 0;
            span.obj_end = 0;
          }
          // Register quantifier
          if (isQuantifier) {
            span.quantifier_index = lineI;
            if (!registerQuantifier(span, subjType, objType, subjMono, objMono)) {
              fprintf(stderr, "WARNING: Too many quantifiers; skipping at word %u\n", lineI);
            }
          }
          break;
        case 8:  // Auxilliary information
          for (auto iter = field.begin(); iter != field.end(); ++iter) {
            switch (*iter) {
              case 'l':
              case 'L':
                isLocationMask[lineI] = true;
                break;
              default:
                fprintf(stderr, "ERROR: Invalid flag to CoNLL constructor: %c", *iter);
            }
          }
          break;
      }
      fieldI += 1;
    }
    if (fieldI != 3 && fieldI != 8 && fieldI != 9) {
      fprintf(stderr, "ERROR: Bad number of CoNLL fields in line (expected 3, 8 or 9. Was %u): %s\n", fieldI, line.c_str());
    }
    lineI += 1;
  }
  
  // Initialize cached variables
  for (uint8_t tokenI = 0; tokenI < MAX_QUERY_LENGTH; ++tokenI) {
    memset(quantifiersInScope[tokenI], MAX_QUANTIFIER_COUNT, MAX_QUANTIFIER_COUNT * sizeof(uint8_t));
    populateQuantifiersInScope(tokenI);
  }
}

//
// Tree::foreachQuantifier()
//
void Tree::foreachQuantifier(
      const uint8_t& index,
      std::function<void(const quantifier_type,const monotonicity)> visitor) const {
  for (uint8_t i = 0; i < MAX_QUANTIFIER_COUNT; ++i) {
    // Get the quantifier in scipe
    uint8_t quantifier = this->quantifiersInScope[index][i];
    if (quantifier >= MAX_QUANTIFIER_COUNT) { return; }
    // Check if it's the subject or object
    const quantifier_span& span = this->quantifierSpans[quantifier];
    bool onSubject = index < span.subj_end && index >= span.subj_begin;
    // Visit the quantifier
    if (onSubject) {
      visitor(this->quantifierMonotonicities[quantifier].subj_type, 
              this->quantifierMonotonicities[quantifier].subj_mono);
    } else {
      visitor(this->quantifierMonotonicities[quantifier].obj_type,
              this->quantifierMonotonicities[quantifier].obj_mono);
    }
  }
}

//
// Tree::populateQuantifiersInScope()
//
#pragma GCC push_options  // matches pop_options below
#pragma GCC optimize ("unroll-loops")
void Tree::populateQuantifiersInScope(const uint8_t index) {
  // Variables
  uint8_t distance[MAX_QUANTIFIER_COUNT];
  memset(distance, 0, MAX_QUANTIFIER_COUNT * sizeof(uint8_t));
  bool valid[MAX_QUANTIFIER_COUNT];
  memset(valid, 0, MAX_QUANTIFIER_COUNT * sizeof(uint8_t));
  // Collect statistics
  for (uint8_t i = 0; i < MAX_QUANTIFIER_COUNT; ++i) {
    if (i >= numQuantifiers) { break; }
    if (index >= quantifierSpans[i].subj_begin && index < quantifierSpans[i].subj_end) {
      valid[i] = true;
      const uint8_t d1 = index - quantifierSpans[i].subj_begin;
      const uint8_t d2 = quantifierSpans[i].subj_end - index;
      distance[i] = (d1 < d2) ? d1 : d2;
    } else if (index >= quantifierSpans[i].obj_begin && index < quantifierSpans[i].obj_end) {
      valid[i] = true;
      const uint8_t d1 = index - quantifierSpans[i].obj_begin;
      const uint8_t d2 = quantifierSpans[i].obj_end - index;
      distance[i] = (d1 < d2) ? d1 : d2;
    }
  }
  // Run foreach
  uint8_t outIndex = 0;
  bool somethingValid = true;
  while (somethingValid) {
    somethingValid = false;
    uint8_t argmin = 255;
    uint8_t min = 255;
    // (get smallest distance from token)
    for (uint8_t i = 0; i < MAX_QUANTIFIER_COUNT; ++i) {
      if (valid[i]) {
        somethingValid = true;
        if (distance[i] < min) {
          min = distance[i];
          argmin = i;
        }
      }
    }
    // (call visitor)
    if (somethingValid) {
      this->quantifiersInScope[index][outIndex] = argmin;
      outIndex += 1;
    }
    // (update state)
    valid[argmin] = false;
  }
  this->quantifiersInScope[index][outIndex] = MAX_QUANTIFIER_COUNT;
}
#pragma GCC pop_options  // matches push_options above
  
//
// Tree::dependents()
//
void Tree::dependents(const uint8_t& index,
      const uint8_t& maxChildren,
      uint8_t* childrenIndices, 
      dep_label* childrenRelations, 
      uint8_t* childrenLength) const {
  *childrenLength = 0;
  for (uint8_t i = 0; i < length; ++i) {
    if (data[i].governor == index) {
      childrenRelations[(*childrenLength)] = data[i].relation;
      childrenIndices[(*childrenLength)++] = i;  // must be last line in block
      if (*childrenLength >= maxChildren) { return; }
    }
  }
}
  
//
// Tree::createDeleteMask()
//
uint32_t Tree::createDeleteMask(const uint8_t& root) const {
  uint32_t bitmask = TREE_DELETE(0x0, root);
  bool cleanPass = false;
  while (!cleanPass) {
    cleanPass = true;
    for (uint8_t i = 0; i < length; ++i) {
      if (!TREE_IS_DELETED(bitmask, i) &&
          data[i].governor != TREE_ROOT &&
          TREE_IS_DELETED(bitmask, data[i].governor)) {
        bitmask = TREE_DELETE(bitmask, i);
        cleanPass = false;
      }
    }
  }
  return bitmask;
}
  
//
// Tree::root()
//
uint8_t Tree::root() const {
  for (uint8_t i = 0; i < length; ++i) {
    if (data[i].governor == TREE_ROOT) {
      return i;
    }
  }
  fprintf(stderr, "No root found in tree!\n");
  std::exit(1);
  return 255;
}
  
//
// Tree::operator==()
//
bool Tree::operator==(const Tree& rhs) const {
  if (length != rhs.length) { return false; }
  for (uint8_t i = 0; i < length; ++i) {
    if (data[i].word != rhs.data[i].word) { return false; }
    if (data[i].governor != rhs.data[i].governor) { return false; }
    if (data[i].relation != rhs.data[i].relation) { return false; }
  }
  return true;
}

//
// Tree::hash()
//
uint64_t Tree::hash() const {
  uint64_t value = 0x0;
  // Hash edges
  for (uint8_t i = 0; i < length; ++i) {
    value ^= hashEdge(edgeInto(i));
  }
  // Hash quantifiers
  value ^= hashQuantifiers(this->quantifierMonotonicities);
  return value;
}
  
//
// Tree::updateHashFromMutation()
//
uint64_t Tree::updateHashFromMutation(
                                      const uint64_t& oldHash,
                                      const uint8_t& index, 
                                      const ::word& oldWord,
                                      const ::word& governor,
                                      const ::word& newWord) const {
  uint64_t newHash = oldHash;
  // Fix incoming dependency
  newHash ^= hashEdge(edgeInto(index, oldWord, governor));
  newHash ^= hashEdge(edgeInto(index, newWord, governor));
  // Fix outgoing dependencies
  for (uint8_t i = 0; i < length; ++i) {
    if (data[i].governor == index) {
      newHash ^= hashEdge(edgeInto(i, data[i].word, oldWord));
      newHash ^= hashEdge(edgeInto(i, data[i].word, newWord));
    }
  }
  // Return
  return newHash;
}

//
// Tree::updateHashFromDeletion()
//
uint64_t Tree::updateHashFromDeletions(
                                       const uint64_t& oldHash,
                                       const uint8_t& deletionIndex, 
                                       const ::word& deletionWord,
                                       const ::word& governor,
                                       const uint32_t& newDeletions) const {
  uint64_t newHash = oldHash;
  for (uint8_t i = 0; i < length; ++i) {
    if (TREE_IS_DELETED(newDeletions, i)) {
      if (i == deletionIndex) {
        // Case: we are deleting the root of the deletion chunk
        newHash ^= hashEdge(edgeInto(i, deletionWord, governor));
      } else {
        // Case: we are deleting an entire edge
        newHash ^= hashEdge(edgeInto(i));
      }
    }
  }
  return newHash;
}
  
//
// Tree::projectLexicalRelation()
//
natlog_relation Tree::projectLexicalRelation( const SearchNode& currentNode,
                                              const natlog_relation& lexicalRelation,
                                              const uint8_t& index) const {
  const dep_tree_word& token = data[index];
  natlog_relation outputRelation = lexicalRelation;
  for (uint8_t i = 0; i < MAX_QUANTIFIER_COUNT; ++i) {
    // Get the quantifier in scope
    uint8_t quantifier = this->quantifiersInScope[index][i];
    if (quantifier >= MAX_QUANTIFIER_COUNT) { break; }
    // Check if it's the subject or object
    const quantifier_span& span = this->quantifierSpans[quantifier];
    bool onSubject = index < span.subj_end && index >= span.subj_begin;
    // Visit the quantifier
    if (onSubject) {
      const uint8_t type = currentNode.quantifierMonotonicities[quantifier].subj_type;
      const uint8_t mono = currentNode.quantifierMonotonicities[quantifier].subj_mono;
      outputRelation = project(mono, type, outputRelation);
    } else {
      const uint8_t type = currentNode.quantifierMonotonicities[quantifier].obj_type;
      const uint8_t mono = currentNode.quantifierMonotonicities[quantifier].obj_mono;
      outputRelation = project(mono, type, outputRelation);
    }
  }
  return outputRelation;
}
  

//
// Tree::projectLexicalRelation()
//
natlog_relation Tree::projectLexicalRelation( const SearchNode& currentNode,
                                              const natlog_relation& lexicalRelation) const {
  return projectLexicalRelation(currentNode, lexicalRelation, currentNode.tokenIndex());
}

//
// Tree::topologicalSortIgnoreQuantifiers
//
void Tree::topologicalSort(uint8_t* buffer, const bool& ignoreQuantifiers) const {
  // Asserts
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
  assert (this->length < 256);  // Should be a noop, type checked by the uint8_t
  #pragma clang diagnostic pop

  // The stack
  uint8_t stack[256];
  stack[0] = this->root();
  uint16_t stackSize = 1;
  // THe output
  uint8_t bufferLength = 0;
  // Temporary variables
  uint8_t childrenBuffer[256];
  dep_label childrenRelBuffer[256];

  // Search!
  // (yo dawg, I heard you like search, so I'm gonna put a search in your search
  //  so you can search while you search).
  while (stackSize > 0) {
      // Pop the node
    uint8_t node = stack[stackSize - 1];
    stackSize -= 1;
    if (node != 255) {  // Just in case we have a sentence of length 256
      // Add the node
      if (!ignoreQuantifiers || !isQuantifier(node)) {
        buffer[bufferLength] = node;
        bufferLength += 1;
        if (bufferLength >= 255) {
          break;
        }
      }
      // Add the children
      uint8_t childrenLength = 0;
      this->dependents(node, childrenBuffer, childrenRelBuffer, &childrenLength);
      for (uint8_t i = 0; i < childrenLength; ++i) {
        stack[stackSize] = childrenBuffer[i];
        stackSize += 1;
      }
    }
  }
  buffer[bufferLength] = 255;
}


// ----------------------------------------------
// NATURAL LOGIC
// ----------------------------------------------

//
// reverseTransition()
//
bool reverseTransition(const bool& endState,
                       const natlog_relation projectedRelation) {
  if (endState) {
    switch (projectedRelation) {
      case FUNCTION_FORWARD_ENTAILMENT:
      case FUNCTION_REVERSE_ENTAILMENT:
      case FUNCTION_EQUIVALENT:
      case FUNCTION_INDEPENDENCE:
        return true;
      case FUNCTION_ALTERNATION:  // negate anyways, just at high cost
      case FUNCTION_NEGATION:
      case FUNCTION_COVER:
        return false;
      default:
        fprintf(stderr, "Unknown function: %u", projectedRelation);
        std::exit(1);
        break;
    }
  } else {
    switch (projectedRelation) {
      case FUNCTION_FORWARD_ENTAILMENT:
      case FUNCTION_REVERSE_ENTAILMENT:
      case FUNCTION_EQUIVALENT:
      case FUNCTION_INDEPENDENCE:
        return false;
      case FUNCTION_COVER:  // negate anyways, just at high cost
      case FUNCTION_NEGATION:
      case FUNCTION_ALTERNATION:
        return true;
      default:
        fprintf(stderr, "Unknown function: %u", projectedRelation);
        std::exit(1);
        break;
    }
  }
  fprintf(stderr, "Reached end of reverseTransition() function -- this shouldn't happen!\n");
  std::exit(1);
  return false;
}

//
// transition()
//
bool transition(const bool& startState,
                const natlog_relation projectedRelation) {
  if (startState) {
    switch (projectedRelation) {
      case FUNCTION_FORWARD_ENTAILMENT:
      case FUNCTION_REVERSE_ENTAILMENT:
      case FUNCTION_EQUIVALENT:
      case FUNCTION_INDEPENDENCE:
        return true;
      case FUNCTION_ALTERNATION:
      case FUNCTION_NEGATION:
      case FUNCTION_COVER:  // negate anyways, just at high cost
        return false;
      default:
        fprintf(stderr, "Unknown function: %u", projectedRelation);
        std::exit(1);
        break;
    }
  } else {
    switch (projectedRelation) {
      case FUNCTION_FORWARD_ENTAILMENT:
      case FUNCTION_REVERSE_ENTAILMENT:
      case FUNCTION_EQUIVALENT:
      case FUNCTION_INDEPENDENCE:
        return false;
      case FUNCTION_COVER:
      case FUNCTION_NEGATION:
      case FUNCTION_ALTERNATION:  // negate anyways, just at high cost
        return true;
      default:
        fprintf(stderr, "Unknown function: %u", projectedRelation);
        std::exit(1);
        break;
    }
  }
  fprintf(stderr, "Reached end of reverseTransition() function -- this shouldn't happen!\n");
  std::exit(1);
  return false;
}

//
// project()
//
uint8_t project(const monotonicity& monotonicity,
                const quantifier_type& quantifierType,
                const natlog_relation& lexicalFunction) {
  switch (monotonicity) {
    case MONOTONE_UP:
      switch (quantifierType) {
        case QUANTIFIER_TYPE_NONE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_ALTERNATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_COVER: return FUNCTION_INDEPENDENCE;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_ADDITIVE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_COVER;
            case FUNCTION_ALTERNATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_COVER: return FUNCTION_COVER;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_MULTIPLICATIVE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_ALTERNATION;
            case FUNCTION_ALTERNATION: return FUNCTION_ALTERNATION;
            case FUNCTION_COVER: return FUNCTION_INDEPENDENCE;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_BOTH:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_NEGATION;
            case FUNCTION_ALTERNATION: return FUNCTION_ALTERNATION;
            case FUNCTION_COVER: return FUNCTION_COVER;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        default: fprintf(stderr, "Unknown projectivity type for quantifier: %u", quantifierType); std::exit(1); break;
      }
    case MONOTONE_DOWN:
      switch (quantifierType) {
        case QUANTIFIER_TYPE_NONE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_ALTERNATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_COVER: return FUNCTION_INDEPENDENCE;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_ADDITIVE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_ALTERNATION;
            case FUNCTION_ALTERNATION: return FUNCTION_INDEPENDENCE;
            case FUNCTION_COVER: return FUNCTION_ALTERNATION;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_MULTIPLICATIVE:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_COVER;
            case FUNCTION_ALTERNATION: return FUNCTION_COVER;
            case FUNCTION_COVER: return FUNCTION_INDEPENDENCE;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        case QUANTIFIER_TYPE_BOTH:
          switch (lexicalFunction) {
            case FUNCTION_EQUIVALENT: return FUNCTION_EQUIVALENT;
            case FUNCTION_FORWARD_ENTAILMENT: return FUNCTION_REVERSE_ENTAILMENT;
            case FUNCTION_REVERSE_ENTAILMENT: return FUNCTION_FORWARD_ENTAILMENT;
            case FUNCTION_NEGATION: return FUNCTION_NEGATION;
            case FUNCTION_ALTERNATION: return FUNCTION_COVER;
            case FUNCTION_COVER: return FUNCTION_ALTERNATION;
            case FUNCTION_INDEPENDENCE: return FUNCTION_INDEPENDENCE;
            default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
          }
          break;
        default: fprintf(stderr, "Unknown projectivity type for quantifier: %u", quantifierType); std::exit(1); break;
      }
      break;
    case MONOTONE_FLAT:
      switch (lexicalFunction) {
        case FUNCTION_EQUIVALENT:
          return FUNCTION_EQUIVALENT;
        case FUNCTION_FORWARD_ENTAILMENT:
        case FUNCTION_REVERSE_ENTAILMENT:
        case FUNCTION_NEGATION:
        case FUNCTION_ALTERNATION:
        case FUNCTION_COVER:
        case FUNCTION_INDEPENDENCE:
          return FUNCTION_INDEPENDENCE;
        default: fprintf(stderr, "Unknown lexical relation: %u", lexicalFunction); std::exit(1); break;
      }
      break;
    default:
      fprintf(stderr, "Invalid monotonicity: %u", monotonicity);
      std::exit(1);
      break;
  }
  fprintf(stderr, "Reached end of project() function -- this shouldn't happen!\n");
  std::exit(1);
  return 255;
}

//
// SynSearch::mutationCost()
//
float SynSearchCosts::mutationCost(const Tree& tree,
                                   const SearchNode& currentNode,
                                   const uint8_t& edgeType,
                                   const bool& endTruthValue,
                                   bool* beginTruthValue) const {
  const natlog_relation lexicalRelation = edgeToLexicalFunction(edgeType);
  // Get the lexical cost of the relation
  const float lexicalRelationCost = mutationLexicalCost[edgeType];
  assert (lexicalRelationCost == lexicalRelationCost);
  assert (lexicalRelationCost >= 0.0);
  const natlog_relation projectedFunction
    = tree.projectLexicalRelation(currentNode, lexicalRelation);
  *beginTruthValue = reverseTransition(endTruthValue, projectedFunction);
  // Get the transition cost of the function
  const float transitionCost
    = ((*beginTruthValue) ? transitionCostFromTrue : transitionCostFromFalse)[projectedFunction];
  assert (transitionCost == transitionCost);
  assert (transitionCost >= 0.0);
  const float totalCost = lexicalRelationCost + transitionCost;
  assert (totalCost == totalCost);
  assert (totalCost >= 0.0);
  return totalCost;
}

//
// SynSearch::insertionCost()
//
float SynSearchCosts::insertionCost(const Tree& tree,
                                    const SearchNode& governor,
                                    const dep_label& dependencyLabel,
                                    const ::word& dependent,
                                    const bool& endTruthValue,
                                    bool* beginTruthValue) const {
  const natlog_relation lexicalRelation
    = dependencyInsertToLexicalFunction(dependencyLabel, dependent);
  const float lexicalRelationCost = insertionLexicalCost[dependencyLabel];
  const natlog_relation projectedFunction
    = tree.projectLexicalRelation(governor, lexicalRelation);
  *beginTruthValue = reverseTransition(endTruthValue, projectedFunction);
  const float transitionCost
    = ((*beginTruthValue) ? transitionCostFromTrue : transitionCostFromFalse)[projectedFunction];
  return lexicalRelationCost + transitionCost;
}

//
// createStrictCosts()
//
SynSearchCosts* createStrictCosts(const float& smallConstantCost,
                                  const float& okCost,
                                  const float& badCost) {
  SynSearchCosts* costs = new SynSearchCosts();
  // Set Constant Costs
  for (uint8_t i = 0; i < NUM_MUTATION_TYPES; ++i) {
    costs->mutationLexicalCost[i] = smallConstantCost;
  }
  for (uint8_t i = 0; i < NUM_DEPENDENCY_LABELS; ++i) {
    costs->insertionLexicalCost[i] = smallConstantCost;
    costs->deletionLexicalCost[i] = smallConstantCost;
  }
  // Tweak 'fishy' mutation types
  costs->mutationLexicalCost[VERB_ENTAIL] = badCost;
  costs->mutationLexicalCost[ANGLE_NN]    = badCost;
  // Set NatLog
  costs->transitionCostFromTrue[FUNCTION_EQUIVALENT] = okCost;
  costs->transitionCostFromTrue[FUNCTION_FORWARD_ENTAILMENT] = okCost;
  costs->transitionCostFromTrue[FUNCTION_REVERSE_ENTAILMENT] = badCost;
  costs->transitionCostFromTrue[FUNCTION_NEGATION] = okCost;
  costs->transitionCostFromTrue[FUNCTION_ALTERNATION] = okCost;
  costs->transitionCostFromTrue[FUNCTION_COVER] = badCost;
  costs->transitionCostFromTrue[FUNCTION_INDEPENDENCE] = badCost;
  costs->transitionCostFromFalse[FUNCTION_EQUIVALENT] = okCost;
  costs->transitionCostFromFalse[FUNCTION_FORWARD_ENTAILMENT] = badCost;
  costs->transitionCostFromFalse[FUNCTION_REVERSE_ENTAILMENT] = okCost;
  costs->transitionCostFromFalse[FUNCTION_NEGATION] = okCost;
  costs->transitionCostFromFalse[FUNCTION_ALTERNATION] = badCost;
  costs->transitionCostFromFalse[FUNCTION_COVER] = okCost;
  costs->transitionCostFromFalse[FUNCTION_INDEPENDENCE] = badCost;
  return costs;
}


////
//// ForwardPartialSearch
////
//void ForwardPartialSearch(
//    const BidirectionalGraph* mutationGraph,
//    const Tree* input,
//    std::function<void(const SearchNode&)> callback) {
//  // Initialize Search
//  vector<SearchNode> fringe;
//  fringe.push_back(SearchNode(*input));
//  uint8_t  dependentIndices[8];
//  natlog_relation  dependentRelations[8];
//  SynSearchCosts* guidingCosts = strictNaturalLogicCosts();
//  bool newTruthValue;
//  btree::btree_set<uint64_t> seen;
//
//  // Run Search
//  while (fringe.size() > 0) {
//    const SearchNode node = fringe.back();
//    fringe.pop_back();
//    if (seen.find(node.factHash()) == seen.end()) { 
////      fprintf(stderr, "  %s\n", toString(*(mutationGraph->impl), *input, node).c_str());
//      callback(node); 
//    }
//    seen.insert(node.factHash());
//
//    // Valid Mutations
//    const vector<edge> edges = mutationGraph->outgoingEdges(node.token());
//    for (auto iter = edges.begin(); iter != edges.end(); ++iter) {
//      // TODO(gabor) NER raising would go here
//    }
//
//    // Valid Deletions
//    uint8_t numDependents;
//    input->dependents(node.tokenIndex(), 8, dependentIndices,
//                    dependentRelations, &numDependents);
//    for (uint8_t dependentI = 0; dependentI < numDependents; ++dependentI) {
//      const uint8_t& dependentIndex = dependentIndices[dependentI];
//      if (node.isDeleted(dependentIndex)) { continue; }
//      const float cost = guidingCosts->deletionCost(
//            *input, node, input->relation(dependentIndex),
//            input->word(dependentIndex), node.truthState(), &newTruthValue);
//      // Deletion (optional)
//      if (newTruthValue && !isinf(cost)) {  // TODO(gabor) handle negation deletions?
//        // (compute)
//        uint32_t deletionMask = input->createDeleteMask(dependentIndex);
//        uint64_t newHash = input->updateHashFromDeletions(
//            node.factHash(), dependentIndex, input->token(dependentIndex).word,
//            node.word(), deletionMask);
//        // (create child)
//        const SearchNode deletedChild(node, newHash, newTruthValue, deletionMask, 0);
//        fringe.push_back(deletedChild);
//      }
//      // Move (always)
//      const SearchNode indexMovedChild(node, *input, dependentIndex, 0);
//      fringe.push_back(indexMovedChild);
//    }
//  }
//
//  delete guidingCosts;
//}


