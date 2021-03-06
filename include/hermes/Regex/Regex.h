/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- C++ -*-
//===--------------------------- regex ------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef HERMES_REGEX_COMPILER_H
#define HERMES_REGEX_COMPILER_H

#include "hermes/Support/Algorithms.h"
#include "hermes/Support/Compiler.h"

#include "hermes/Regex/RegexBytecode.h"
#include "hermes/Regex/RegexNode.h"
#include "hermes/Regex/RegexTypes.h"

#include <string>
#include <vector>

namespace hermes {
namespace regex {

template <class Traits>
class Regex {
  // Enable the Parser to add nodes to us.
  template <class A, class B>
  friend class Parser;

  using CharT = typename Traits::CodeUnit;
  using CodePoint = typename Traits::CodePoint;
  using Node = regex::Node;
  using BracketNode = regex::BracketNode<Traits>;

 private:
  Traits traits_;
  SyntaxFlags flags_ = {};

  // Number of capture groups encountered so far.
  uint32_t markedCount_ = 0;

  // Number of loops encountered so far.
  uint32_t loopCount_ = 0;

  // The list of nodes so far.
  NodeList nodes_;

  // The error, which may be set after parsing.
  constants::ErrorType error_ = constants::ErrorType::None;

  // Constraints on the type of strings that can match this regex.
  MatchConstraintSet matchConstraints_ = 0;

  /// Construct and and append a node of type NodeType at the end of the nodes_
  /// list. The node should be constructible from \p args.
  /// \return an observer pointer to the new node.
  template <typename NodeType, typename... Args>
  NodeType *appendNode(Args &&... args) {
    std::unique_ptr<NodeType> node =
        hermes::make_unique<NodeType>(std::forward<Args>(args)...);
    NodeType *nodePtr = node.get();
    nodes_.push_back(std::move(node));
    return nodePtr;
  }

  /// \return the "current" node, which is the last (rightmost) node created.
  Node *currentNode() {
    return nodes_.back().get();
  }

  /// \return the number of marked subexpressions.
  uint32_t markedCount() const {
    return markedCount_;
  }

  /// \increment the number of marked subexpressions and return the value.
  uint32_t incrementMarkedCount() {
    return ++markedCount_;
  }

  /// Given that the node \p splicePoint is in our node list, remove all nodes
  /// after it. \return a list of the removed nodes.
  NodeList spliceOut(Node *splicePoint) {
    assert(splicePoint && "null node in spliceOut");
    // Find the index of the splice point. We expect it to be towards the end.
    size_t spliceIndex = nodes_.size();
    while (spliceIndex--) {
      if (nodes_[spliceIndex].get() == splicePoint)
        break;
    }
    assert(spliceIndex < nodes_.size() && "Node not in node list");
    // Move all nodes after the splice index into a new vector.
    // Note this may be empty.
    auto firstToMove = nodes_.begin() + spliceIndex + 1;
    NodeList result;
    std::move(firstToMove, nodes_.end(), std::back_inserter(result));
    nodes_.erase(firstToMove, nodes_.end());
    return result;
  }

 public:
  /// Compile the regex into bytecode. Return the resulting bytecode.
  std::vector<uint8_t> compile() const {
    assert(valid() && "Cannot compile invalid regex.");
    // TODO: add validation for the loop and reduce the size of markedCount_ and
    // loopCount_ to uint16_t.
    assert(
        markedCount_ <= constants::kMaxCaptureGroupCount &&
        "Too many capture groups");
    assert(loopCount_ <= constants::kMaxLoopCount && "Too many loops");
    RegexBytecodeHeader header = {static_cast<uint16_t>(markedCount_),
                                  static_cast<uint16_t>(loopCount_),
                                  flags_.toByte(),
                                  matchConstraints_};
    RegexBytecodeStream bcs(header);
    Node::compile(nodes_, bcs);
    return bcs.acquireBytecode();
  }

  // Constructors
  Regex() = default;
  explicit Regex(const CharT *p, const llvm::ArrayRef<char16_t> f = {})
      : Regex(p, p + std::char_traits<CharT>::length(p), f) {}

  Regex(
      const CharT *first,
      const CharT *last,
      const llvm::ArrayRef<char16_t> flags = {}) {
    // Compute the SyntaxFlags based on the flags string.
    auto sflags = SyntaxFlags::fromString(flags);
    if (!sflags) {
      error_ = constants::ErrorType::InvalidFlags;
      return;
    }
    flags_ = *sflags;
    error_ = parse(first, last);
  }

  // Disallow copy-assignment and copy-construction.
  Regex &operator=(const Regex &) = delete;
  Regex(const Regex &) = delete;

  /// Move-assignment and move-construction.
  Regex &operator=(Regex &&) = default;
  Regex(Regex &&) = default;

  // Accessors.
  unsigned markCount() const {
    return markedCount_;
  }
  SyntaxFlags flags() const {
    return flags_;
  }

  /// \return any errors produced during parsing, or ErrorType::None if none.
  constants::ErrorType getError() const {
    return error_;
  }

  /// \return whether the regex was parsed successfully.
  bool valid() const {
    return error_ == constants::ErrorType::None;
  }

  /// \return the set of match constraints for the regex.
  MatchConstraintSet matchConstraints() const {
    return matchConstraints_;
  }

 private:
  template <class ForwardIterator>
  constants::ErrorType parse(ForwardIterator first, ForwardIterator last);

  /// Attempt to parse the regex from the range [\p first, \p last), using
  /// \p backRefLimit as the maximum decimal escape to interpret as a
  /// backreference.  The maximum backreference that was in fact encountered
  /// is returned by reference in \p out_max_back_ref, if that is larger than
  /// its current value. \return an error code.
  template <class ForwardIterator>
  constants::ErrorType parseWithBackRefLimit(
      ForwardIterator first,
      ForwardIterator last,
      uint32_t backRefLimit,
      uint32_t *outMaxBackRef);
  void pushLeftAnchor();
  void pushRightAnchor();
  void pushMatchAny();
  void pushLoop(
      uint32_t min,
      uint32_t max,
      NodeList loopedList,
      uint32_t mexp_begin,
      bool greedy);
  BracketNode *startBracketList(bool negate);
  void pushChar(CodePoint c);
  void pushCharClass(CharacterClass c);
  void pushBackRef(uint32_t i);
  void pushAlternation(std::vector<NodeList> alternatives);
  void pushMarkedSubexpression(NodeList, uint32_t mexp);
  void pushWordBoundary(bool);
  void pushLookaround(NodeList, uint16_t, uint16_t, bool, bool);
};

/// Node for lookaround assertions like (?=...) and (?!...)
class LookaroundNode : public Node {
  using Super = Node;

  /// The contained expression representing our lookaround assertion.
  NodeList exp_;

  /// Match constraints for our contained expression.
  MatchConstraintSet expConstraints_;

  /// Whether the lookaround assertion is negative (?!) or positive (?=).
  const bool invert_;

  /// Whether the lookaround is forwards (true) or backwards (false).
  const bool forwards_;

  /// The marked subexpressions contained within this lookaround.
  uint16_t mexpBegin_;
  uint16_t mexpEnd_;

 public:
  LookaroundNode(
      NodeList exp,
      uint32_t mexpBegin,
      uint32_t mexpEnd,
      bool invert,
      bool forwards)
      : exp_(move(exp)),
        expConstraints_(matchConstraintsForList(exp_)),
        invert_(invert),
        forwards_(forwards),
        mexpBegin_(mexpBegin),
        mexpEnd_(mexpEnd) {
    // Clear AnchoredAtStart for lookbehind assertions.
    // For example:
    //    /(?<=^abc)def/.exec("abcdef")
    // this matches the substring "def", even though that substring is not
    // anchored at the start.
    if (!forwards_) {
      expConstraints_ &= ~MatchConstraintAnchoredAtStart;
    }
  }

  virtual MatchConstraintSet matchConstraints() const override {
    // Positive lookarounds apply their match constraints.
    // e.g. if our assertion is anchored at the start, so are we.
    MatchConstraintSet result = 0;
    if (!invert_) {
      result |= expConstraints_;
    }
    // Lookarounds match an empty string even if their contents do not.
    result &= ~MatchConstraintNonEmpty;
    return result | Super::matchConstraints();
  }

  virtual void optimizeNodeContents(SyntaxFlags flags) override {
    optimizeNodeList(exp_, flags);
  }

  // Override emit() to compile our lookahead expression.
  virtual void emit(RegexBytecodeStream &bcs) const override {
    auto lookaround = bcs.emit<LookaroundInsn>();
    lookaround->invert = invert_;
    lookaround->forwards = forwards_;
    lookaround->constraints = expConstraints_;
    lookaround->mexpBegin = mexpBegin_;
    lookaround->mexpEnd = mexpEnd_;
    compile(exp_, bcs);
    lookaround->continuation = bcs.currentOffset();
  }
};

template <typename Receiver>
constants::ErrorType parseRegex(
    const char16_t *start,
    const char16_t *end,
    Receiver *receiver,
    SyntaxFlags flags,
    uint32_t backRefLimit,
    uint32_t *outMaxBackRef);

template <class Traits>
template <class ForwardIterator>
constants::ErrorType Regex<Traits>::parse(
    ForwardIterator first,
    ForwardIterator last) {
  uint32_t maxBackRef = 0;
  auto result = parseWithBackRefLimit(
      first, last, constants::kMaxCaptureGroupCount, &maxBackRef);

  // Validate loop and capture group count.
  if (markedCount_ > constants::kMaxCaptureGroupCount ||
      loopCount_ > constants::kMaxLoopCount) {
    return constants::ErrorType::PatternExceedsParseLimits;
  }

  // See comment --DecimalEscape--
  // We parsed without a backreference limit because we had to parse to discover
  // the limit. Now we know that we wrongly interpreted a decimal escape as a
  // backreference. See ES6 Annex B.1.4 DecimalEscape "but only if the integer
  // value DecimalEscape is <= NCapturingParens". Now that we know the true
  // capture group count, either produce an error (if Unicode) or re-parse with
  // that as the limit so overlarge decimal escapes will be ignored.
  if (result == constants::ErrorType::None && maxBackRef > markedCount_) {
    if (flags_.unicode) {
      return constants::ErrorType::EscapeInvalid;
    }

    uint32_t backRefLimit = markedCount_;
    uint32_t reparsedMaxBackRef = 0;
    loopCount_ = 0;
    markedCount_ = 0;
    matchConstraints_ = 0;
    result =
        parseWithBackRefLimit(first, last, backRefLimit, &reparsedMaxBackRef);
    assert(
        result == constants::ErrorType::None &&
        "regex reparsing should never fail if the first parse succeeded");
    assert(
        reparsedMaxBackRef <= backRefLimit &&
        "invalid backreference generated");
    (void)reparsedMaxBackRef;
  }
  return result;
}

template <class Traits>
template <class ForwardIterator>
constants::ErrorType Regex<Traits>::parseWithBackRefLimit(
    ForwardIterator first,
    ForwardIterator last,
    uint32_t backRefLimit,
    uint32_t *outMaxBackRef) {
  // Initialize our node list with a single no-op node (it must never be empty.)
  nodes_.clear();
  nodes_.push_back(hermes::make_unique<Node>());
  auto result =
      parseRegex(first, last, this, flags_, backRefLimit, outMaxBackRef);

  // If we succeeded, add a goal node as the last node and perform optimizations
  // on the list.
  if (result == constants::ErrorType::None) {
    nodes_.push_back(hermes::make_unique<GoalNode>());
    Node::optimizeNodeList(nodes_, flags_);
  }

  // Compute any match constraints.
  matchConstraints_ = Node::matchConstraintsForList(nodes_);

  return result;
}

template <class Traits>
void Regex<Traits>::pushLoop(
    uint32_t min,
    uint32_t max,
    NodeList loopedExpr,
    uint32_t mexp_begin,
    bool greedy) {
  appendNode<LoopNode>(
      loopCount_++,
      min,
      max,
      greedy,
      mexp_begin,
      markedCount_,
      move(loopedExpr));
}

template <class Traits>
void Regex<Traits>::pushChar(CodePoint c) {
  bool icase = flags().ignoreCase;
  if (icase)
    c = traits_.canonicalize(c, flags().unicode);
  appendNode<MatchCharNode>(Node::CodePointList{c}, flags());
}

template <class Traits>
void Regex<Traits>::pushCharClass(CharacterClass c) {
  auto bracket = startBracketList(false);
  bracket->addClass(c);
}

template <class Traits>
void Regex<Traits>::pushMarkedSubexpression(NodeList nodes, uint32_t mexp) {
  appendNode<MarkedSubexpressionNode>(std::move(nodes), mexp);
}

template <class Traits>
void Regex<Traits>::pushLeftAnchor() {
  appendNode<LeftAnchorNode>(flags());
}

template <class Traits>
void Regex<Traits>::pushRightAnchor() {
  appendNode<RightAnchorNode>();
}

template <class Traits>
void Regex<Traits>::pushMatchAny() {
  appendNode<MatchAnyNode>(flags());
}

template <class Traits>
void Regex<Traits>::pushWordBoundary(bool invert) {
  appendNode<WordBoundaryNode>(invert);
}

template <class Traits>
void Regex<Traits>::pushBackRef(uint32_t i) {
  appendNode<BackRefNode>(i);
}

template <class Traits>
void Regex<Traits>::pushAlternation(std::vector<NodeList> alternatives) {
  appendNode<AlternationNode>(std::move(alternatives));
}

template <class Traits>
BracketNode<Traits> *Regex<Traits>::startBracketList(bool negate) {
  return appendNode<BracketNode>(traits_, negate, flags_);
}

template <class Traits>
void Regex<Traits>::pushLookaround(
    NodeList exp,
    uint16_t mexpBegin,
    uint16_t mexpEnd,
    bool invert,
    bool forwards) {
  if (!forwards) {
    Node::reverseNodeList(exp);
  }
  exp.push_back(hermes::make_unique<GoalNode>());
  appendNode<LookaroundNode>(
      std::move(exp), mexpBegin, mexpEnd, invert, forwards);
}

void Node::reverseNodeList(NodeList &nodes) {
  // If we have a goal node it must come at the end.
#ifndef NDEBUG
  for (const auto &node : nodes) {
    assert(
        (!node->isGoal() || (node == nodes.back())) &&
        "Goal node should only be at end");
  }
#endif

  // Reverse this list, excluding any terminating goal.
  if (!nodes.empty()) {
    bool hasGoal = nodes.back()->isGoal();
    std::reverse(nodes.begin(), nodes.end() - (hasGoal ? 1 : 0));
  }

  // Recursively reverse child nodes.
  for (auto &node : nodes) {
    node->reverseChildren();
  }
}

void Node::optimizeNodeList(NodeList &nodes, SyntaxFlags flags) {
  // Recursively optimize child nodes.
  for (auto &node : nodes) {
    node->optimizeNodeContents(flags);
  }

  // Merge adjacent runs of char nodes.
  // For example, [CharNode('a') CharNode('b') CharNode('c')] becomes
  // [CharNode('abc')].
  for (size_t idx = 0, max = nodes.size(); idx < max; idx++) {
    // Get the range of nodes that can be successfully coalesced.
    Node::CodePointList chars;
    size_t rangeStart = idx;
    size_t rangeEnd = idx;
    for (; rangeEnd < max; rangeEnd++) {
      if (!nodes[rangeEnd]->tryCoalesceCharacters(&chars)) {
        break;
      }
    }
    if (rangeEnd - rangeStart >= 3) {
      // We successfully coalesced some nodes.
      // Replace the range with a new node.
      nodes[rangeStart] = std::unique_ptr<MatchCharNode>(
          new MatchCharNode(std::move(chars), flags));
      // Fill the remainder of the range with null (we'll clean them up after
      // the loop) and skip to the end of the range.
      // Note that rangeEnd may be one past the last valid element.
      std::fill(
          nodes.begin() + (rangeStart + 1), nodes.begin() + rangeEnd, nullptr);
      idx = rangeEnd - 1;
    }
  }

  // Remove any nulls that we introduced.
  nodes.erase(std::remove(nodes.begin(), nodes.end(), nullptr), nodes.end());
}

} // namespace regex
} // namespace hermes

#endif // HERMES_REGEX_COMPILER_H
