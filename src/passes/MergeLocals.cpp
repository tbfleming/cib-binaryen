/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Merges locals when it is beneficial to do so.
//
// An obvious case is when locals are copied. In that case, two locals have the
// same value in a range, and we can pick which of the two to use. For
// example, in
//
//  (if (result i32)
//   (tee_local $x
//    (get_local $y)
//   )
//   (i32.const 100)
//   (get_local $x)
//  )
// 
// If that assignment of $y is never used again, everything is fine. But if
// if is, then the live range of $y does not end in that get, and will
// necessarily overlap with that of $x - making them appear to interfere
// with each other in coalesce-locals, even though the value is identical.
//
// To fix that, we replace uses of $y with uses of $x. This extends $x's
// live range and shrinks $y's live range. This tradeoff is not always good,
// but $x and $y definitely overlap already, so trying to shrink the overlap
// makes sense - if we remove the overlap entirely, we may be able to let
// $x and $y be coalesced later.
//
// If we can remove only some of $y's uses, then we are definitely not
// removing the overlap, and they do conflict. In that case, it's not clear
// if this is beneficial or not, and we don't do it for now
// TODO: investigate more
//

#include <wasm.h>
#include <pass.h>
#include <wasm-builder.h>
#include <ir/local-graph.h>

namespace wasm {

struct MergeLocals : public WalkerPass<PostWalker<MergeLocals, UnifiedExpressionVisitor<MergeLocals>>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new MergeLocals(); }

  void doWalkFunction(Function* func) {
    // first, instrument the graph by modifying each copy
    //   (set_local $x
    //    (get_local $y)
    //   )
    // to
    //   (set_local $x
    //    (tee_local $y
    //     (get_local $y)
    //    )
    //   )
    // That is, we add a trivial assign of $y. This ensures we
    // have a new assignment of $y at the location of the copy,
    // which makes it easy for us to see if the value if $y
    // is still used after that point
    super::doWalkFunction(func);

    // optimize the copies, merging when we can, and removing
    // the trivial assigns we added temporarily
    optimizeCopies();
  }

  std::vector<SetLocal*> copies;

  void visitSetLocal(SetLocal* curr) {
    if (auto* get = curr->value->dynCast<GetLocal>()) {
      if (get->index != curr->index) {
        Builder builder(*getModule());
        auto* trivial = builder.makeSetLocal(get->index, get);
        curr->value = trivial;
        copies.push_back(curr);
      }
    }
  }

  void optimizeCopies() {
    // compute all dependencies
    LocalGraph localGraph(getFunction(), getModule());
    localGraph.computeInfluences();
    // optimize each copy
    for (auto* copy : copies) {
      auto* trivial = copy->value->cast<SetLocal>();
      bool canDoThemAll = true;
      for (auto* influencedGet : localGraph.setInfluences[trivial]) {
        // this get uses the trivial write, so it uses the value in the copy.
        // however, it may depend on other writes too, if there is a merge/phi,
        // and in that case we can't do anything
        assert(influencedGet->index == trivial->index);
        if (localGraph.getSetses[influencedGet].size() == 1) {
          // this is ok
          assert(*localGraph.getSetses[influencedGet].begin() == trivial);
        } else {
          canDoThemAll = false;
        }
      }
      if (canDoThemAll) {
        // worth it for this copy, do it
        for (auto* influencedGet : localGraph.setInfluences[trivial]) {
          influencedGet->index = copy->index;
        }
      }
      // either way, get rid of the trivial get
      copy->value = trivial->value;
    }
  }
};

Pass *createMergeLocalsPass() {
  return new MergeLocals();
}

} // namespace wasm
