#include "function.h"
#include "compapi.h"
#include "scopeguard.h"
#include "util.h"

using namespace std;
using namespace llvm;
using namespace ante::parser;

namespace ante {

/*
 *  Adds llvm attributes to an Argument based off the parameters type
 */
void addArgAttrs(llvm::Argument &arg, AnType *argTy){
    if(argTy->typeTag == TT_Function){
        arg.addAttr(Attribute::AttrKind::NoCapture);
    }

    if(arg.getType()->isPointerTy() && !argTy->hasModifier(Tok_Mut)){
        arg.addAttr(Attribute::AttrKind::ReadOnly);
    }
}

/*
 *  Same as addArgAttrs, but for every parameter
 */
void addAllArgAttrs(Function *f, AnFunctionType *fnTy){
    size_t i = 0;
    for(auto &arg : f->args()){
        assert(i < fnTy->paramTys.size());
        addArgAttrs(arg, fnTy->paramTys[i]);
        i++;
    }
}

LOC_TY getFinalLoc(Node *n){
    auto *seq = dynamic_cast<SeqNode*>(n);
    if(!seq){
        if(BlockNode* bn = dynamic_cast<BlockNode*>(n)){
            n = bn->block.get();
            seq = dynamic_cast<SeqNode*>(n);
        }
    }
    return seq ? seq->sequence.back()->loc : n->loc;
}


//swap the bodies of the two functions and delete the former.
void moveFunctionBody(Function *src, Function *dest){
    dest->getBasicBlockList().splice(dest->begin(), src->getBasicBlockList());
    src->getBasicBlockList().clearAndLeakNodesUnsafely();
}


vector<llvm::Argument*> buildArguments(FunctionType *ft){
    vector<llvm::Argument*> args;
    for(unsigned i = 0, e = ft->getNumParams(); i != e; i++){
        assert(!ft->getParamType(i)->isVoidTy() && "Cannot have void typed arguments!");
        args.push_back(new llvm::Argument(ft->getParamType(i)));
    }
    return args;
}


/*
 *  Handles the modifiers or compiler directives (eg. ![inline]) then
 *  compiles the function fdn with either compFn or compLetBindingFn.
 */
TypedValue compFnWithModifiers(Compiler *c, FuncDecl *fd){
    //remove the preproc node at the front of the modifier list so that the call to
    //compFn does not call this function in an infinite loop
    auto *fdn = fd->getFDN();
    auto mod = fdn->modifiers.back().release();
    fdn->modifiers.pop_back();

    TypedValue fn;
    if(mod->isCompilerDirective()){
        if(VarNode *vn = dynamic_cast<VarNode*>(mod->directive.get())){
            if(vn->name == "inline"){
                fn = c->compFn(fd);
                if(!fn) return fn;
                ((Function*)fn.val)->addFnAttr(Attribute::AttrKind::AlwaysInline);
            }else if(vn->name == "on_fn_decl"){
                auto *rettn = (TypeNode*)fdn->returnType.get();
                auto *fnty = AnFunctionType::get(toAnType(rettn, c->compUnit), fdn->params.get(), c->compUnit);
                fn = TypedValue(nullptr, fnty);
            }else{
                fdn->modifiers.emplace_back(mod);
                error("Unrecognized compiler directive '"+vn->name+"'", vn->loc);
            }

            return fn;
        }else{
            fdn->modifiers.emplace_back(mod);
            error("Unrecognized compiler directive", mod->loc);
            return {};
        }
    // ppn is a normal modifier
    }else{
        if(mod->mod == Tok_Ante){
            if(c->isJIT){
                if(capi::lookup(fd->getName())){
                    fn = fd->tval.val ? fd->tval : c->compFn(fd);
                    //Tag as TT_MetaFunction
                    fd->tval.val = fn.val;
                }else{
                    fn = c->compFn(fd);
                }
            }else{
                auto *rettn = (TypeNode*)fdn->returnType.get();
                AnType *fnty;
                fnty = AnFunctionType::get(toAnType(rettn, c->compUnit), fdn->params.get(), c->compUnit);
                if(!capi::lookup(fd->getName())){
                    fnty = (AnType*)fnty->addModifier(Tok_Ante);
                }
                fn = TypedValue(nullptr, fnty);
            }
        }else{
            fn = c->compFn(fd);
        }
        fdn->modifiers.emplace_back(mod);
        return fn;
    }
}


/**
 * Removes compile-time-only parameters and wraps each mut type in a pointer.
 */
AnFunctionType* removeCTParamsAndWrapMutParams(Compiler *c, AnType *functy){
    auto ft = static_cast<AnFunctionType*>(functy);

    vector<AnType*> params;
    params.reserve(ft->paramTys.size());
    for(auto &param : ft->paramTys){
        // remove empty params but include varargs
        if(!isEmptyType(c, param) || param->isRowVar()){
            if(param->hasModifier(Tok_Mut)){
                params.push_back(AnPtrType::get(param));
            }else{
                params.push_back(param);
            }
        }
    }
    return AnFunctionType::get(ft->retTy, params, {});
}


vector<NamedValNode*> vectorize(NamedValNode *n){
    std::vector<NamedValNode*> result;
    for(auto& nvn : *n){
        result.push_back(static_cast<NamedValNode*>(&nvn));
    }
    return result;
}


TypedValue compFnHelper(Compiler *c, FuncDecl *fd){
    BasicBlock *caller = c->builder.GetInsertBlock();
    auto *fdn = fd->getFDN();

    if(!fdn->modifiers.empty()){
        auto ret = compFnWithModifiers(c, fd);
        c->builder.SetInsertPoint(caller);
        return ret;
    }

    AnFunctionType *fnTy = try_cast<AnFunctionType>(fdn->getType());
    auto fnTyNoCtParams = removeCTParamsAndWrapMutParams(c, fnTy);

    FunctionType *ft = dyn_cast<FunctionType>(c->anTypeToLlvmType(fnTyNoCtParams)->getPointerElementType());
    Function *f = Function::Create(ft, Function::ExternalLinkage, fd->getName(), c->module.get());
    addAllArgAttrs(f, fnTy);

    TypedValue ret{f, fnTy};
    fd->tval.val = f;

    //The above handles everything for a function declaration
    //If the function is a definition, then the body will be compiled here.
    if(fdn->child){
        //Create the entry point for the function
        BasicBlock *bb = BasicBlock::Create(*c->ctxt, "entry", f);
        c->builder.SetInsertPoint(bb);

        //iterate through each parameter and add its value to the new scope.
        auto curParam = fdn->params.get();
        for(auto &arg : f->args()){
            curParam->decl->tval.val = &arg;
            curParam = static_cast<NamedValNode*>(curParam->next.get());
        }

        //Compile the function body, and hold onto the last value
        TypedValue v;
        try{
            v = CompilingVisitor::compile(c, fdn->child);
        }catch(CtError const& e){
            c->builder.SetInsertPoint(caller);
            throw e;
        }

        //push the final value as a return, explicit returns are already added in RetNode::compile
        if(!dyn_cast<ReturnInst>(v.val)){
            auto loc = getFinalLoc(fdn->child.get());

            if(fnTy->retTy->typeTag == TT_Unit){
                c->builder.CreateRetVoid();
            }else{
                v.val = c->builder.CreateRet(v.val);
            }
        }
    }

    c->builder.SetInsertPoint(caller);
    return ret;
}


void CompilingVisitor::visit(FuncDeclNode *n){
    // Only lambdas need to be compiled immediately.
    // Other functions can be done lazily when called.
    if(n->name.empty() && n->decl->isFuncDecl()){
        val = c->compFn(static_cast<FuncDecl*>(n->decl));
    }
}


//Provide a wrapper for function-compiling methods so that each
//function is compiled in its own isolated module
TypedValue Compiler::compFn(FuncDecl *fd){
    compCtxt->callStack.push_back(fd);
    auto *continueLabels = compCtxt->continueLabels.release();
    auto *breakLabels = compCtxt->breakLabels.release();
    compCtxt->continueLabels = std::make_unique<vector<BasicBlock*>>();
    compCtxt->breakLabels = std::make_unique<vector<BasicBlock*>>();

    TMP_SET(this->fnScope, this->scope);
    TypedValue ret = compFnHelper(this, fd);

    compCtxt->callStack.pop_back();
    compCtxt->continueLabels.reset(continueLabels);
    compCtxt->breakLabels.reset(breakLabels);
    return ret;
}


FuncDecl* Compiler::getCurrentFunction() const{
    return compCtxt->callStack.back();
}

} //end of namespace ante
