package org.goobs.truth

import Learn._
import org.goobs.truth.EdgeType._
import edu.stanford.nlp.stats.ClassicCounter
import org.goobs.truth.Messages._

import edu.stanford.nlp.util.logging.Redwood.Util._
import edu.stanford.nlp.Sentence
import edu.smu.tspell.wordnet.{Synset, WordNetDatabase, SynsetType}
import org.goobs.sim.{SearchState, Search}


object NatLog {
  lazy val wordnet:WordNetDatabase = WordNetDatabase.getFileInstance

  /**
   * The actual implementing call for soft and hard NatLog weights.
   */
  def natlogWeights(jcWeight:Double, positiveWeight:Double, negativeWeight:Double, dontCareWeight:Double):WeightVector = {
    val weights = new ClassicCounter[String]
    if (jcWeight > 0) { throw new IllegalArgumentException("Weights must always be negative (jcWeight is not)"); }
    if (positiveWeight > 0) { throw new IllegalArgumentException("Weights must always be negative (positiveWeight is not)"); }
    if (negativeWeight > 0) { throw new IllegalArgumentException("Weights must always be negative (negativeWeight is not)"); }
    if (dontCareWeight > 0) { throw new IllegalArgumentException("Weights must always be negative (dontCareWeight is not)"); }
    // Set negative weight
    weights.setDefaultReturnValue(negativeWeight)
    // Set positive weights
    // (unigrams)
    weights.setCount(unigramUp(   WORDNET_UP    ), jcWeight)
    weights.setCount(unigramUp(   FREEBASE_UP   ), jcWeight)
    weights.setCount(unigramDown( WORDNET_DOWN  ), jcWeight)
    weights.setCount(unigramDown( FREEBASE_DOWN ), jcWeight)
    // (bigrams)
    weights.setCount(bigramUp( WORDNET_UP, WORDNET_UP ), positiveWeight)
    weights.setCount(bigramUp( WORDNET_UP, FREEBASE_UP ), positiveWeight)
    weights.setCount(bigramUp( FREEBASE_UP, WORDNET_UP ), positiveWeight)
    weights.setCount(bigramUp( FREEBASE_UP, FREEBASE_UP ), positiveWeight)
    weights.setCount(bigramDown( WORDNET_DOWN, WORDNET_DOWN ), positiveWeight)
    weights.setCount(bigramDown( WORDNET_DOWN, FREEBASE_DOWN ), positiveWeight)
    weights.setCount(bigramDown( FREEBASE_DOWN, WORDNET_DOWN ), positiveWeight)
    weights.setCount(bigramDown( FREEBASE_DOWN, FREEBASE_DOWN ), positiveWeight)
    // Set "don't care" weights
    weights.setCount(unigramAny( MORPH_TO_LEMMA ),    -0.0)
    weights.setCount(unigramAny( MORPH_FROM_LEMMA ),  -0.0)
    weights.setCount(unigramAny( MORPH_FUDGE_NUMBER), -0.0)
    // Set weights we only care about a bit
    weights.setCount(unigramUp(ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    weights.setCount(unigramDown(ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    weights.setCount(unigramFlat(ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    weights.setCount(bigramUp(ANGLE_NEAREST_NEIGHBORS, ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    weights.setCount(bigramDown(ANGLE_NEAREST_NEIGHBORS, ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    weights.setCount(bigramFlat(ANGLE_NEAREST_NEIGHBORS, ANGLE_NEAREST_NEIGHBORS), dontCareWeight)
    // Return
    weights
  }

  /**
   * The naive NatLog hard constraint weights
   */
  def hardNatlogWeights:WeightVector = natlogWeights(-1.0, 0.0, Double.NegativeInfinity, Double.NegativeInfinity)

  /**
   * A soft initialization to NatLog weights; this is the same as
   * hardNatlogWeights, but with a soft rather than hard penalty for invalid
   * weights.
   * The goal is to use this to initialize the search.
   */
  def softNatlogWeights:WeightVector = natlogWeights(-1.0, 0.0, -1.0, -0.25)

  /**
   * Determine the monotonicity of a sentence, according to the quantifier it starts with.
   * @param sentence The sentence, in surface form. It should start with the quantifier (for now).
   * @return A pair of integers, one for each of the two quantifier arguments, for positive (>0), negative (<0) or flat(=0)
   *         monotonicity.
   */
  private def monotonicityMarking(sentence:String):(Int, Int) = {
    val lower = sentence.toLowerCase.replaceAll( """\s+""", " ")
    if (lower.startsWith("all ") ||
        lower.startsWith("any ") ||
        lower.startsWith("every ")    ) {
      (-1, +1)
    } else if (lower.startsWith("most ") ||
               lower.startsWith("enough ") ||
               lower.startsWith("few ") ||
               lower.startsWith("lots of ")) {
      (0, +1)
    } else if (lower.contains(" dont ") ||
               lower.contains(" don't ") ||
               lower.contains(" do not ") ||
               lower.contains(" are not ") ||
               lower.startsWith("no ") ||
               lower.startsWith("none of ")) {
      (-1, -1)
    } else if (lower.startsWith("some ") ||
               lower.startsWith("there are ") ||
               lower.startsWith("there exists ") ||
               lower.startsWith("there exist ")) {
      (+1, +1)
    } else {
      (-1, +1)  // default to something like "almost all", approximated as "all"
    }
  }

   /**
    * Compute the Lesk overlap between a synset and a sentence.
    * For example, the value given the sentences [a, b, c, d] and [a, c, d, e]
    * would be 1^2 2^2 = 5 (for 'a' and 'c d').
    */
  def lesk(a:Synset, b:Array[String], approx:Boolean=true):Double = {
    import Search._
    def allEqual(a:Array[String], startA:Int,
                 b:Array[String], startB:Int, length:Int):Boolean = {
      (0 until length).forall{ (i:Int) => a(startA + i) == b(startB + i) }
    }
    def allFalse(mask:Array[Boolean], start:Int, untilVal:Int):Boolean = {
      (start until untilVal).forall( mask(_) == false )
    }
    // (variables)
    val tokensA:Array[String]
    = a.getDefinition.toLowerCase.split("""\s+""")
    val tokensB:Array[String]
    = b.map( _.toLowerCase )
    val tokensShort = if (tokensA.length < tokensB.length) tokensA else tokensB
    val tokensLong = if (tokensA.length < tokensB.length) tokensB else tokensA
    // (possible alignments)
    var candidates = List[((Array[Boolean],Array[Boolean])=>Boolean,
      AlignState=>AlignState)]()
    for( length <- 1 to tokensB.length;
         shortStart <- 0 to tokensShort.length - length;
         longStart <- 0 to tokensLong.length - length ) {
      if (allEqual(tokensShort, shortStart,
        tokensLong, longStart, length) ) {
        val candidate = (
          (shortMask:Array[Boolean], longMask:Array[Boolean]) => {
            allFalse(shortMask, shortStart, shortStart + length) &&
              allFalse(longMask, longStart, longStart + length)
          },
          (old:AlignState) => {
            val newShortMask = old.shortMask.map( x => x )
            val newLongMask = old.longMask.map( x => x )
            for( i <- shortStart until shortStart + length ) newShortMask(i) = true
            for( i <- longStart until longStart + length ) newLongMask(i) = true
            new AlignState(newShortMask, newLongMask, old.cost - length * length)
          }
          )
        candidates = candidate :: candidates
      }
    }
    // (search)
    case class AlignState(shortMask:Array[Boolean],
                          longMask:Array[Boolean],
                          override val cost:Double) extends SearchState {
      override def children:List[AlignState] = {
        candidates.filter( _._1(shortMask, longMask) )
          .map( _._2(this) )
      }
    }
    val maxCost:Double = tokensShort.size * tokensShort.size + 1.0
    new Search[AlignState](if (approx) GREEDY else cache(UNIFORM_COST))
      .best(AlignState(tokensShort.map( x => false ),
      tokensLong.map( x => false ), maxCost)).cost
  }

  /**
   * Get the best matching word sense for the given word; considering the rest of the 'sentence'.
   * @param word The word to get the synset for.
   * @param sentence The 'sentence' -- that is, containing fact -- of the word.
   * @param pos The 'POS tag' -- that is, synset type -- of the word.
   *            This should be [[None]] if no POS tag is known.
   * @return The most likely Synset; if no information is present to disambiguate, this should return the
   *         first synset that matches the POS tag.
   */
  def getWordSense(word:String, sentence:Sentence, pos:Option[SynsetType]):Int = {
    val synsets:Array[Synset] = wordnet.getSynsets(word)
    if (synsets == null) {
      // Case: sensless
      0
    } else {
      // Case: find WordNet sense
      val (argmax, argmaxIndex) = synsets.zipWithIndex.maxBy{ case (synset:Synset, synsetIndex:Int) =>
        if (pos.isDefined && synset.getType != pos.get) {
          -1000.0 + synsetIndex.toDouble / 100.0
        } else {
          lesk(synset, sentence.words, approx = true) - synsetIndex.toDouble * 1.1
        }
      }
      log(s"disambiguating $word as '${argmax.getDefinition}'")
      math.min(31, argmaxIndex + 1)
    }
  }

  /**
   * Annotate a given fact according to monotonicity (and possibly other flags).
   * This output is ready to send as a query to the search server.
   *
   * @param leftArg Arg 1 of the triple.
   * @param rel The plain-text relation of the triple
   * @param rightArg Arg 2 of the triple.
   * @return An annotated query fact, marked with monotonicity.
   */
  def annotate(leftArg:String, rel:String, rightArg:String):Fact = {
    val index:String=>Array[Int] = {(arg:String) =>
      if (Props.NATLOG_INDEXER_LAZY) {
        Utils.index(arg, doHead = false, allowEmpty = false)(Postgres.indexerContains, Postgres.indexerGet).map(_._1).getOrElse({ warn(s"could not index$arg"); Array[Int]() })
      } else {
        Utils.index(arg, doHead = false, allowEmpty = false)((s:String) => Utils.wordIndexer.containsKey(s), (s:String) => Utils.wordIndexer.get(s)).map(_._1).getOrElse({warn(s"could not index $arg"); Array[Int]() })
      }}

    // Compute Monotonicity
    val (arg1Monotonicity, arg2Monotonicity) = monotonicityMarking(leftArg + " " + rel + " " + rightArg)

    // Tokenize
    val tokens:(Array[Int], Array[Int], Array[Int]) = (index(leftArg), index(rel), index(rightArg))
    val monotoneTaggedTokens:Array[(Int, Int)] =
      (tokens._1.map{ (_, arg1Monotonicity) }.toList ::: tokens._2.map{ (_, arg2Monotonicity) }.toList ::: tokens._3.map{ (_, arg2Monotonicity) }.toList).toArray

    // POS tag
    val sentence:Sentence = Sentence(leftArg + " " + rel + " " + rightArg)
    val words:Array[String] = sentence.words
    val pos:Array[Option[SynsetType]] = {
      // (get variables)
      val pos:Array[String] = sentence.pos
      val chunkedWords:Array[String] = (tokens._1.toList ::: tokens._2.toList ::: tokens._3.toList).toArray
        .map ( (w:Int) => if (Props.NATLOG_INDEXER_LAZY) Postgres.indexerGloss(w) else Utils.wordGloss(w) )
      // (regexps)
      val NOUN = "N.*".r
      val VERB = "V.*".r
      val ADJ  = "J.*".r
      val ADV  = "R.*".r
      // (find synset POS)
      var tokenI = 0
      val synsetPOS:Array[Option[SynsetType]] = Array.fill[Option[SynsetType]](chunkedWords.size)( None )
      for (i <- 0 until words.size) {
        if (!chunkedWords(tokenI).contains(words(i))) {
          tokenI += 1
        }
        if (!chunkedWords(tokenI).contains(words(i))) {
          throw new IllegalStateException("Could not match word " + words(i) + " with any chunk in " + chunkedWords.mkString(" "))
        }
        if (synsetPOS(tokenI) != SynsetType.ALL_TYPES) {
          synsetPOS(tokenI) = pos(i) match {
            case NOUN(_) => Some(SynsetType.NOUN)
            case VERB(_) => Some(SynsetType.VERB)
            case ADJ(_) =>  Some(SynsetType.ADJECTIVE)
            case ADV(_) =>  Some(SynsetType.ADVERB)
            case _ => None
          }
        }
      }
      // (filter unknown)
      synsetPOS
    }

    // Create Protobuf Words
    val protoWords = monotoneTaggedTokens.zip(pos).map{ case ((word:Int, monotonicity:Int), pos:Option[SynsetType]) =>
      var monotonicityValue = Monotonicity.FLAT
      if (monotonicity > 0) { monotonicityValue = Monotonicity.UP }
      if (monotonicity < 0) { monotonicityValue = Monotonicity.DOWN }
      val gloss = if (Props.NATLOG_INDEXER_LAZY) Postgres.indexerGloss(word) else Utils.wordGloss(word)
      Word.newBuilder()
        .setWord(word)
        .setGloss(gloss)
        .setPos(pos match {
        case Some(SynsetType.NOUN) => "n"
        case Some(SynsetType.VERB) => "v"
        case Some(SynsetType.ADJECTIVE) => "j"
        case Some(SynsetType.ADVERB) => "r"
        case _ => "?"
        }).setSense(getWordSense(gloss, sentence, pos))
        .setMonotonicity(monotonicityValue).build()
    }

    // Create Protobuf Fact
    val fact: Fact.Builder = Fact.newBuilder()
    for (word <- protoWords) { fact.addWord(word) }
    fact.setGloss(leftArg + " " + rel + " " + rightArg)
        .setToString("[" + rel + "](" + leftArg + ", " + rightArg + ")")
        .build()
  }
}
