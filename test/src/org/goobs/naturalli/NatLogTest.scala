package org.goobs.naturalli

import scala.collection.JavaConversions._
import edu.smu.tspell.wordnet.WordNetDatabase
import edu.stanford.nlp.natlog.Monotonicity
import edu.stanford.nlp.Sentence

/**
 * Tests for various NatLog functionalities
 *
 * @author gabor
 */
class NatLogTest extends Test {

  val NO_QUANTIFIER = Monotonicity.QUANTIFIERLESS match {
    case Monotonicity.UP => Messages.Monotonicity.UP
    case Monotonicity.DOWN => Messages.Monotonicity.DOWN
    case Monotonicity.NON => Messages.Monotonicity.FLAT
  }

  describe("Natural Logic Weights") {
    Props.NATLOG_INDEXER_LAZY = false
    describe("when a hard assignment") {
      it ("should accept Wordnet monotone jumps") {
        NatLog.hardNatlogWeights.getCount(Evaluate.monoUp_stateTrue(EdgeType.WORDNET_UP)) should be >= -1.0
        NatLog.hardNatlogWeights.getCount(Evaluate.monoDown_stateTrue(EdgeType.WORDNET_DOWN)) should be >= -1.0
      }
      it ("should accept Freebase monotone jumps") {
        NatLog.hardNatlogWeights.getCount(Evaluate.monoUp_stateTrue(EdgeType.FREEBASE_UP)) should be >= -1.0
        NatLog.hardNatlogWeights.getCount(Evaluate.monoDown_stateTrue(EdgeType.FREEBASE_DOWN)) should be >= -1.0
      }
    }
  }


  describe("Monotonicity Markings") {
    Props.NATLOG_INDEXER_LAZY = false
    import Messages.Monotonicity._
    it ("should mark 'all'") {
      NatLog.annotate("all cats", "have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, DOWN, UP, UP))
      NatLog.annotate("every cat", "has", "a tail").getWordList.map( _.getMonotonicity ).toList should be (List(UP, DOWN, UP, UP, UP))
    }
    it ("should mark 'some'") {
      NatLog.annotate("some cats", "have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, UP, UP, UP))
      NatLog.annotate("there are cats", "which have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, UP, UP, UP, UP))
      NatLog.annotate("there exist cats", "which have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, UP, UP, UP, UP))
    }
    it ("should mark 'few'") {
      NatLog.annotate("few cat", "have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, DOWN, UP, UP))
    }
    it ("should mark 'most'/'many'") {
      NatLog.annotate("most cats", "have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, FLAT, UP, UP))
      NatLog.annotate("many cats", "have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(UP, FLAT, UP, UP))
    }
    it ("should mark 'no'") {
      NatLog.annotate("no cats", "have", "tails").getWordList.map(_.getMonotonicity).toList should be(List(UP, DOWN, DOWN, DOWN))
    }
    it ("should mark 'not'") {
      NatLog.annotate("cat", "do not have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(NO_QUANTIFIER, UP, UP, DOWN, DOWN))
      NatLog.annotate("cat", "don't have", "tails").getWordList.map( _.getMonotonicity ).toList should be (List(NO_QUANTIFIER, UP, UP, DOWN, DOWN))
    }
    it ("should work on 'Every job that involves a giant squid is dangerous'") {
      NatLog.annotate("every job that involves a giant squid is dangerous").head.getWordList.map( _.getMonotonicity ).toList should be (
        List(UP, DOWN, DOWN, DOWN, DOWN, DOWN, UP, UP))
    }
    it ("should work on 'Not every job that involves a giant squid is safe'") {
      new Sentence("not every job that involves a giant squid is safe").words.length should be (10)
      NatLog.annotate("not every job that involves a giant squid is safe").head.getWordList.map( _.getMonotonicity ).toList should be (
        List(UP, DOWN, UP, UP, UP, UP, UP, DOWN, DOWN))
    }
  }

  describe("Monotone boundaries") {
    it("should find the first verb") {
      NatLog.annotate("some cat has a tail").head.getMonotoneBoundary should be (2)
      NatLog.annotate("all cats have a tail").head.getMonotoneBoundary should be (2)
      NatLog.annotate("all cats are animals").head.getMonotoneBoundary should be (2)

    }
  }

  describe("Lesk") {
    Props.NATLOG_INDEXER_LAZY = false
    it ("should be perfect for exact string matches") {
      NatLog.lesk(WordNetDatabase.getFileInstance.getSynsets("cat")(0), "feline mammal usually having thick soft fur and no ability to roar: domestic cats; wildcats".split("""\s+""")) should be (225.0)
    }
    it ("should be reasonable for multi-word overlaps") {
      NatLog.lesk(WordNetDatabase.getFileInstance.getSynsets("cat")(5), "cat be tracked vehicle".split("""\s+""")) should be (4.0)
      NatLog.lesk(WordNetDatabase.getFileInstance.getSynsets("cat")(5), "cat be large tracked vehicle".split("""\s+""")) should be (9.0)
    }
  }

  describe("Word Senses") {
    Props.NATLOG_INDEXER_LAZY = false
    val CAT_ANIMAL = 1
    val ANIMAL = 1
    val HAVE = 2
    val TAIL_ANIMAL = 2
    val CAT_VEHICLE = 6
    val BE = 9
    val LARGE = 2
    val TRACKED_VEHICLE = 0 // TODO(gabor) this should not be 0  (lemmatization doesn't play nice with multiword expressions)
    val FLORIDA = 0  // TODO(gabor) not entirely sure why this is 0?
    val AMERICAN_STATE = 0  // TODO(gabor) same lemmatization problem as with TRACKED_VEHICLE
    val FINISH = 10
    val ON_TIME = 2
    it("should get default sense of 'cat'") {
      NatLog.annotate("the cat", "have", "tail").getWordList.map(_.getPos).toList should be(List("e", "n", "v", "n"))
      NatLog.annotate("the cat", "have", "tail").getWordList.map(_.getSense).toList should be(List(0, CAT_ANIMAL, HAVE, TAIL_ANIMAL))
    }
    it("should get vehicle senses of 'CAT' with enough evidence") {
      NatLog.annotate("the cat", "be", "large tracked vehicle").getWordList.map(_.getPos).toList should be(List("e", "n", "v", "j", "n"))
      NatLog.annotate("the cat", "be", "large tracked vehicle").getWordList.map(_.getSense).toList should be(List(0, CAT_VEHICLE, BE, LARGE, TRACKED_VEHICLE))
    }
    it("should get right sense of 'tail'") {
      NatLog.annotate("some cat", "have", "tail").getWordList.map(_.getSense).toList should be(List(0, CAT_ANIMAL, HAVE, TAIL_ANIMAL))
      NatLog.annotate("some animal", "have", "tail").getWordList.map(_.getSense).toList should be(List(0, ANIMAL, HAVE, TAIL_ANIMAL))
    }
    it("should not mark final VBP as a OTHER") {
      NatLog.annotate("cats", "have", "more fur than dogs have").getWordList.map(_.getPos).toList should be(List("n", "v", "j", "n", "?", "n", "?"))
      NatLog.annotate("cats", "have", "a more important role than dogs are").getWordList.map(_.getPos).toList.last should be("?")
    }
    it("should handle senses with lemmatization") {
      NatLog.annotate("Florida is in American state").head.getWordList.map(_.getPos).toList should be(List("n", "v", "?", "n"))
      NatLog.annotate("Florida is in American state").head.getWordList.map(_.getSense).toList should be(List(FLORIDA, BE, 0, AMERICAN_STATE))
      NatLog.annotate("Florida be in American state").head.getWordList.map(_.getPos).toList should be(List("n", "v", "?", "n"))
      NatLog.annotate("Florida be in American state").head.getWordList.map(_.getSense).toList should be(List(FLORIDA, BE, 0, AMERICAN_STATE))
    }
    it("should handle senses for mult-word expressions") {
      NatLog.annotate("the cat finished on time").head.getWordList.map(_.getSense).toList should be(List(0, CAT_ANIMAL, FINISH, ON_TIME))
    }
  }
  describe("NER Tags") {
    it ("should be abstracted if unknown word") {
      val savedRepl = Props.NATLOG_INDEXER_REPLNER
      Props.NATLOG_INDEXER_REPLNER = true
      NatLog.annotate("Chris Manning is a professor").head.getWordList.map( _.getGloss ).toList should be (List("person", "be", "a", "professor"))
      NatLog.annotate("Chris Manning advises Sonal Gupta").head.getWordList.map( _.getGloss ).toList should be (List("person", "advise", "person"))
      Props.NATLOG_INDEXER_REPLNER = savedRepl
    }
    it ("should not be abstracted if known word") {
      val savedRepl = Props.NATLOG_INDEXER_REPLNER
      Props.NATLOG_INDEXER_REPLNER = true
      NatLog.annotate("George Bush is a president").head.getWordList.map( _.getGloss ).toList should be (List("george bush", "be", "a", "president"))
      NatLog.annotate("George Bush met Chris Manning").head.getWordList.map( _.getGloss ).toList should be (List("george bush", "meet", "person"))
      Props.NATLOG_INDEXER_REPLNER = savedRepl
    }
    it ("should not be abstracted if unknown word") {
      val savedRepl = Props.NATLOG_INDEXER_REPLNER
      Props.NATLOG_INDEXER_REPLNER = false
      NatLog.annotate("Chris Manning is a professor").head.getWordList.map( _.getGloss ).toList should be (List(Utils.WORD_UNK, "be", "a", "professor"))
      NatLog.annotate("Chris Manning advises Sonal Gupta").head.getWordList.map( _.getGloss ).toList should be (List(Utils.WORD_UNK, "advise", Utils.WORD_UNK, "gupta"))
      Props.NATLOG_INDEXER_REPLNER = savedRepl
    }
    //    it ("should lemmatize before abstracting") {
    //      val savedRepl = Props.NATLOG_INDEXER_REPLNER
    //      Props.NATLOG_INDEXER_REPLNER = true
    //      //(from ACE)
    //      NatLog.annotate("Nazis save Jews").head.getWordList.map( _.getGloss ).toList should be (List("nazi", "save", "jew"))
    //      Props.NATLOG_INDEXER_REPLNER = savedRepl
    //    }

  }
}