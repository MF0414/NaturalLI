#include "Trie.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <iostream>

#ifdef HAVE_UNORDERED_MAP
  #include <unordered_map>
#else
  #include <map>
#endif

#ifdef HAVE_BOOST_THREAD
  #include <boost/thread/thread.hpp>
  #include <boost/lockfree/queue.hpp>
  #include <boost/atomic.hpp>
#endif

#include "Postgres.h"
#include "Utils.h"




using namespace std;
using namespace btree;

//
// Trie::~Trie
//
Trie::~Trie() {
  for (btree_map<word,Trie*>::iterator iter = children.begin(); iter != children.end(); ++iter) {
    delete iter->second;
  }
}

//
// Trie::add
//
void Trie::add(const edge* elements, const uint8_t& length,
               const Graph* graph) {
  // Corner cases
  if (length == 0) { return; }  // this case shouldn't actually happen normally...
  // Register child
  const word w = elements[0].source;
  assert (w > 0);
  const btree_map<word,Trie*>::iterator childIter = children.find( w );
  Trie* child = NULL;
  if (childIter == children.end()) {
    child = new Trie();
    children[w] = child;
  } else {
    child = childIter->second;
  }
  // Register information about child
  if (graph == NULL || graph->containsDeletion(elements[0])) {
    child->registerEdge(elements[0]);
  }
  // Recursive call
  if (length == 1) {
    child->data.isLeaf = true;    // Mark this as a leaf node
#if HIGH_MEMORY
    completions[w] = child;  // register a completion
#endif
  } else {
    child->add(&(elements[1]), length - 1, graph);
  }
}

inline uint8_t min(const uint8_t& a, const uint8_t b) { return a < b ? a : b; }

//
// Trie::addCompletion
//
inline void Trie::addCompletion(const Trie* child, const word& source,
                                edge* insertion, uint32_t& index) const {
  if (index < MAX_COMPLETIONS - 4) {
    // case: write directly to insertion buffer
    uint8_t numEdges = child->getEdges(&(insertion[index]));
    for (int i = 0; i < numEdges; ++i) { insertion[index + i].source = source; }
    index += numEdges;
  } else {
    // case: write to temporary buffer and copy over
    edge buffer[4];
    uint8_t bufferedEdges = child->getEdges(buffer);
    uint8_t numEdges = min( MAX_COMPLETIONS - index, bufferedEdges );
    for (int i = 0; i < numEdges; ++i) { buffer[i].source = source; }
    memcpy(&(insertion[index]), buffer, numEdges * sizeof(edge));
    index += numEdges;
  }
}



//
// Trie::contains
//
const bool Trie::contains(const tagged_word* query, 
                          const uint8_t& queryLength,
                          const int16_t& mutationIndex,
                          edge* insertions,
                              uint32_t& mutableIndex) const {
  assert (queryLength > mutationIndex);

  // -- Part 1: Fill in completions --
  if (mutationIndex == -1) {
    const bool tooManyChildren = (children.size() > MAX_COMPLETIONS);
    if (!tooManyChildren) {
      // sub-case: add all children
      btree_map<word,Trie*>::const_iterator iter;
      for (iter = children.begin(); iter != children.end(); ++iter) {
        addCompletion(iter->second, iter->first, insertions, mutableIndex);
        if (mutableIndex >= MAX_COMPLETIONS) { break; }
      }
    } else {
#if HIGH_MEMORY
      // sub-case: too many children; only add completions
      for (btree_map<word,Trie*>::const_iterator iter = completions.begin(); iter != completions.end(); ++iter) {
        addCompletion(iter->second, iter->first, insertions, mutableIndex);
        if (mutableIndex >= MAX_COMPLETIONS) { break; }
      }
#endif
    }
  }
  
  // -- Part 2: Check containment --
  if (queryLength == 0) {
    // return whether the fact exists
    return isLeaf();
  } else {
    // Case: we're in the middle of the query
    btree_map<word,Trie*>::const_iterator childIter = children.find( query[0].word );
    if (childIter == children.end()) {
      // Return false
      return false;
    } else {
      // Check the child
      return childIter->second->contains(&(query[1]), 
                                         queryLength - 1,
                                         mutationIndex - 1,
                                         insertions,
                                         mutableIndex);
    }
  }
}

//
// Trie::memoryUsage
//
uint64_t Trie::memoryUsage(uint64_t* onFacts,
                           uint64_t* onStructure,
                           uint64_t* onCompletionCaching) const {
  // (make sure variables work)
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  if (onFacts == NULL) {
    onFacts = &a;
  }
  if (onStructure == NULL) {
    onStructure = &b;
  }
  if (onCompletionCaching == NULL) {
    onCompletionCaching = &c;
  }
  // (me)
  (*onStructure) += sizeof(*this);
  // (completions)
#if HIGH_MEMORY
  (*onCompletionCaching) += (sizeof(word) + sizeof(Trie*)) * completions.size();
#endif
  // (children)
  for (btree_map<word,Trie*>::const_iterator childIter = children.begin();
       childIter != children.end(); ++childIter) {
    (*onFacts) += sizeof(word);
    (*onStructure) += sizeof(Trie*);
    childIter->second->memoryUsage(onFacts, onStructure, onCompletionCaching);
  }
  // (return)
  return (*onFacts) + (*onStructure) + (*onCompletionCaching);
}

//
// ----------------------------------------------------------------------------
//

//
// TrieRoot::add()
//
void TrieRoot::add(const edge* elements, const uint8_t& length,
                   const Graph* graph) {
  // Add the fact
  Trie::add(elements, length, graph);
  // Register skip-gram
  const word w = elements[0].source;
  if (length > 1) {
    const word grandChildW = elements[1].source;
    assert (grandChildW > 0);
    skipGrams[grandChildW].push_back(w);
  }
}

//
// TrieRoot::contains
//
const bool TrieRoot::contains(const tagged_word* query, 
                              const uint8_t& queryLength,
                              const int16_t& mutationIndex,
                              edge* insertions) const {
  assert (queryLength > mutationIndex);
  uint32_t mutableIndex = 0;
  bool contains;
  if (mutationIndex == -1) {
    if (queryLength > 0) {
      btree_map<word,vector<word>>::const_iterator skipGramIter = skipGrams.find( query[0].word );
      if (skipGramIter != skipGrams.end()) {
        // Case: add anything that leads into the second term
        for (vector<word>::const_iterator iter = skipGramIter->second.begin(); iter != skipGramIter->second.end(); ++iter) {
          btree_map<word,Trie*>::const_iterator childIter = children.find( *iter );
          if (childIter != children.end()) {
            addCompletion(childIter->second, childIter->first, insertions, mutableIndex);
          }
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      } else {
        // Case: we're kind of shit out of luck. We're inserting into the
        //       beginning of the sentence, but with no valid skip-grams.
        //       So, let's just add some starting words and pray.
        for (btree_map<word,Trie*>::const_iterator childIter = children.begin(); childIter != children.end(); ++childIter) {
          addCompletion(childIter->second, childIter->first, insertions, mutableIndex);
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      }
    } else {
      // Case: add any single-term completions
      for (btree_map<word,Trie*>::const_iterator iter = children.begin(); iter != children.end(); ++iter) {
        if (iter->second->isLeaf()) {
          addCompletion(iter->second, iter->first, insertions, mutableIndex);
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      }
    }
    contains = Trie::contains(query, queryLength, -9000, insertions, mutableIndex);  // already added completions
  } else {
    contains = Trie::contains(query, queryLength, mutationIndex, insertions, mutableIndex);
  }

  // Return
  if (mutableIndex < MAX_COMPLETIONS) {
    insertions[mutableIndex].source = 0;
  }
  return contains;
}
  
//
// TrieRoot::memoryUsage()
//
uint64_t TrieRoot::memoryUsage(uint64_t* onFacts,
                               uint64_t* onStructure,
                               uint64_t* onCompletionCaching) const {
  // (make sure variables work)
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  if (onFacts == NULL) {
    onFacts = &a;
  }
  if (onStructure == NULL) {
    onStructure = &b;
  }
  if (onCompletionCaching == NULL) {
    onCompletionCaching = &c;
  }
  Trie::memoryUsage(onFacts, onStructure, onCompletionCaching);
  // (skip-grams)
  for (btree_map<word,vector<word>>::const_iterator skipGramIter = skipGrams.begin();
       skipGramIter != skipGrams.end(); ++skipGramIter) {
    (*onCompletionCaching) += sizeof(word);
    (*onCompletionCaching) += sizeof(vector<word>) + sizeof(word) * skipGramIter->second.size();
  }
  return (*onFacts) + (*onStructure) + (*onCompletionCaching);
}

//
// ----------------------------------------------------------------------------
//

inline bool consumeRow(Trie* facts, PGRow& row,
                       const Graph* graph,
#ifdef HAVE_UNORDERED_MAP
                       unordered_map<word,vector<edge>> word2senses
#else
                       map<word,vector<edge>> word2senses
#endif
    ) {
  const char* gloss = row[0];
  uint32_t weight = atoi(row[1]);
  if (weight < MIN_FACT_COUNT) { return false; }
  // Parse fact
  stringstream stream (&gloss[1]);
  string substr;
  edge buffer[256];
  uint8_t bufferLength = 0;
  while( getline (stream, substr, ',' ) ) {
    // Parse the word
    word w = atoi(substr.c_str());
    // Register the word
#ifdef HAVE_UNORDERED_MAP
    unordered_map<word,vector<edge>>::const_iterator iter = word2senses.find( w );
#else
    map<word,vector<edge>>::const_iterator iter = word2senses.find( w );
#endif
    if (iter == word2senses.end() || iter->second.size() == 0) {
      buffer[bufferLength].source       = w;
      buffer[bufferLength].source_sense = 0;
      buffer[bufferLength].type         = 0;
      buffer[bufferLength].cost         = 1.0f;
    } else {
      buffer[bufferLength] = iter->second[0];
    }
    buffer[bufferLength].sink       = 0;
    buffer[bufferLength].sink_sense = 0;
    if (bufferLength >= MAX_FACT_LENGTH) { break; }
    bufferLength += 1;
  }
  // Error check
  if (cin.bad()) {
    printf("IO Error: %s\n", gloss);
    std::exit(2);
  }
  // Add fact
  // Add 'canonical' version
  facts->add(buffer, bufferLength, graph);
  // Add word sense variants
  for (uint32_t k = 0; k < bufferLength; ++k) {
#ifdef HAVE_UNORDERED_MAP
    unordered_map<word,vector<edge>>::iterator iter = word2senses.find( buffer[k].source );
#else
    map<word,vector<edge>>::iterator iter = word2senses.find( buffer[k].source );
#endif
    if (iter != word2senses.end() && iter->second.size() > 1) {
      for (uint32_t sense = 1; sense < iter->second.size(); ++sense) {
        buffer[k] = iter->second[sense];
        facts->add(buffer, bufferLength, graph);
      }
    }
  }
  // continue loop
  return true;
}

//
// ReadFactTrie
//
FactDB* ReadFactTrie(const uint64_t maxFactsToRead, const Graph* graph) {
  Trie* facts = new TrieRoot();
  char query[127];

  // Read valid deletions
  printf("Reading registered deletions...\n");
#ifdef HAVE_UNORDERED_MAP
  unordered_map<word,vector<edge>> word2senses;
#else
  map<word,vector<edge>> word2senses;
#endif
  // (query)
  snprintf(query, 127, "SELECT DISTINCT (source) source, source_sense, type FROM %s WHERE source<>0 AND sink=0 ORDER BY type;", PG_TABLE_EDGE);
  PGIterator wordIter = PGIterator(query);
  uint32_t numValidInsertions = 0;
  while (wordIter.hasNext()) {
    // Get fact
    PGRow row = wordIter.next();
    // Create edge
    edge e;
    e.source       = atoi(row[0]);
    e.source_sense = atoi(row[1]);
    e.type  = atoi(row[2]);
    e.cost  = 1.0f;
    // Register edge
    word2senses[e.source].push_back(e);
    numValidInsertions += 1;
  }
  printf("  Done. %u words have sense tags\n", numValidInsertions);


  // Read facts
  printf("Reading facts...\n");
  // (query)
  if (maxFactsToRead == std::numeric_limits<uint64_t>::max()) {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC;",
             PG_TABLE_FACT);
  } else {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC LIMIT %lu;",
             PG_TABLE_FACT,
             maxFactsToRead);
  }
  PGIterator iter = PGIterator(query);
  uint64_t i = 0;

  // vv begin loop vv
  while (iter.hasNext()) {
    // Get fact
    PGRow row = iter.next();
    if (!consumeRow(facts, row, graph, word2senses)) { break; }
    // Debug
    i += 1;
    if (i % 1000000 == 0) {
      printf("  loaded %luM facts (%luMB memory used in Trie)\n",
             i / 1000000,
             facts->memoryUsage(NULL, NULL, NULL) / 1000000);
    }
  }
  // ^^ end loop ^^

  // Return
  printf("  done reading the fact database (%lu facts read)\n", i);
  return facts;
}
