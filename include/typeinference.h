#ifndef AN_TYPEINFERENCEVISITOR_H
#define AN_TYPEINFERENCEVISITOR_H

#include "parser.h"
#include "antype.h"
#include "constraintfindingvisitor.h"
#include "unification.h"
#include "substitutingvisitor.h"

namespace ante {

    /**
     * Perform type inference on a parse tree.
     * This consists of several steps:
     * 1. Annotate tree with placeholder types  (TypeInferenceVisitor)
     * 2. Recurse again to find a list of constraints  (ConstrintFindingVisitor)
     * 3. Perform unification
     * 4. Substitute any yet-unresolved types  (SubstitutingVisitor)
     */
    struct TypeInferenceVisitor : public NodeVisitor {
        TypeInferenceVisitor(){}

        /** Infer types of all expressions in parse tree and
        * mutate the ast with the inferred types. */
        static void infer(parser::Node *n){
            TypeInferenceVisitor step1;
            n->accept(step1);

            ConstraintFindingVisitor step2;
            n->accept(step2);

            auto constraints = step2.getConstraints();
            auto substitutions = unify(constraints);

            SubstitutingVisitor::substituteIntoAst(n, substitutions);
        }


        static void infer(std::unique_ptr<parser::Node> &n){
            return infer(n.get());
        }

        static void infer(std::shared_ptr<parser::Node> &n){
            return infer(n.get());
        }

        DECLARE_NODE_VISIT_METHODS();
    };
}

#endif
