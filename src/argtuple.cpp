#include "argtuple.h"
#include "types.h"

using namespace std;
using namespace llvm;

namespace ante {
    //ante function to convert between IEEE half and IEEE single
    //since c++ does not support an IEEE half value
#ifndef F16_BOOT
    extern "C" float f32_from_f16(uint16_t f);
#else
    float f32_from_f16(uint16_t f) {
        return f;
    }
#endif

    /**
     * Converts an ArgTuple into a typedValue.  If the type
     * cannot be converted or an error occurs, c's error flag
     * is set and a void literal is returned.
     */
    TypedValue convertToTypedValue(Compiler *c, ArgTuple &arg, AnType *tn){
        auto *data = arg.asRawData();
        switch(tn->typeTag){
            case TT_I8:              return TypedValue(c->builder.getInt8( *(uint8_t*) data), tn);
            case TT_I16:             return TypedValue(c->builder.getInt16(*(uint16_t*)data), tn);
            case TT_I32:             return TypedValue(c->builder.getInt32(*(uint32_t*)data), tn);
            case TT_I64:             return TypedValue(c->builder.getInt64(*(uint64_t*)data), tn);
            case TT_U8:              return TypedValue(c->builder.getInt8( *(uint8_t*) data), tn);
            case TT_U16:             return TypedValue(c->builder.getInt16(*(uint16_t*)data), tn);
            case TT_U32:             return TypedValue(c->builder.getInt32(*(uint32_t*)data), tn);
            case TT_U64:             return TypedValue(c->builder.getInt64(*(uint64_t*)data), tn);
            case TT_Isz:             return TypedValue(c->builder.getIntN( *(size_t*)  data, AN_USZ_SIZE), tn);
            case TT_Usz:             return TypedValue(c->builder.getIntN( *(size_t*)  data, AN_USZ_SIZE), tn);
            case TT_C8:              return TypedValue(c->builder.getInt8( *(uint8_t*) data), tn);
            case TT_C32:             return TypedValue(c->builder.getInt32(*(uint32_t*)data), tn);
            case TT_F16:             return TypedValue(ConstantFP::get(*c->ctxt, APFloat(f32_from_f16(*(uint16_t*)data))), tn);
            case TT_F32:             return TypedValue(ConstantFP::get(*c->ctxt, APFloat(*(float*)data)), tn);
            case TT_F64:             return TypedValue(ConstantFP::get(*c->ctxt, APFloat(*(double*)data)), tn);
            case TT_Bool:            return TypedValue(c->builder.getInt1(*(uint8_t*)data), tn);
            case TT_Tuple:           break;
            case TT_Array:           break;
            case TT_Ptr: {
                auto *cint = c->builder.getIntN(AN_USZ_SIZE, *(size_t*)data);
                auto *ty = c->anTypeToLlvmType(tn);
                return TypedValue(c->builder.CreateIntToPtr(cint, ty), tn);
            }
            case TT_Data:
            case TT_TypeVar:
            case TT_Function:
            case TT_TaggedUnion:
            case TT_MetaFunction:
            case TT_FunctionList:
            case TT_Type:
                break;
            case TT_Void:
                return c->getVoidLiteral();
        }

        c->errFlag = true;
        cerr << "ArgTuple: Unknown/Unimplemented TypeTag " << typeTagToStr(tn->typeTag) << endl;
        return c->getVoidLiteral();
    }

    void ArgTuple::allocAndStoreValue(Compiler *c, TypedValue const& tv){
        auto size = tv.type->getSizeInBits(c);
        if(!size){
            cerr << size.getErr() << endl;
            size = 0;
        }
        data = malloc(size.getVal() / 8);
        storeValue(c, tv);
    }
    
    
    /**
     * Stores a pointer value of a constant pointer type
     */
    void ArgTuple::storePtr(Compiler *c, TypedValue const &tv){
        auto *ptrty = (AnPtrType*)tv.type;

        if(GlobalVariable *gv = dyn_cast<GlobalVariable>(tv.val)){
            Value *v = gv->getInitializer();
            if(ConstantDataArray *cda = dyn_cast<ConstantDataArray>(v)){
                char *cstr = strdup(cda->getAsString().str().c_str());
                *(void**)data = cstr;
            }else{
                TypedValue tv = {v, ptrty->extTy};
                void **oldData = (void**)data;
                data = *oldData;
                allocAndStoreValue(c, tv);
                *oldData = data;
                data = oldData;
            }
        }else if(ConstantExpr *ce = dyn_cast<ConstantExpr>(tv.val)){
            Instruction *in = ce->getAsInstruction();
            if(GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(in)){
                auto ptr = TypedValue(gep->getPointerOperand(), ptrty->extTy);
                storePtr(c, ptr);
            }

        }else if(BitCastInst *be = dyn_cast<BitCastInst>(tv.val)){
            //there should be stores in this bitcast if it was of malloc
            for(auto *u : be->users()){
                if(StoreInst *si = dyn_cast<StoreInst>(u)){
                    //Its possible this is a store of the same type if the pointer is mutable,
                    //we want what is stored within only
                    if(si->getValueOperand()->getType() == tv.val->getType()->getPointerElementType()){
                        TypedValue elem = {si->getValueOperand(), ptrty->extTy};
                        void **data_ptr = (void**)data;
                        allocAndStoreValue(c, elem);
                        *data_ptr = data;
                        data = data_ptr;
                        return;
                    }
                }
            }
        }else{
            cerr << "error: unknown type given to getConstPtr, dumping\n";
            c->errFlag = true;
            tv.dump();
        }
    }


    void ArgTuple::storeTuple(Compiler *c, TypedValue const& tup){
        auto *sty = (AnAggregateType*)tup.type;
        if(ConstantStruct *ca = dyn_cast<ConstantStruct>(tup.val)){
            void *orig_data = this->data;
            for(size_t i = 0; i < ca->getNumOperands(); i++){
                Value *elem = ca->getAggregateElement(i);
                AnType *ty = sty->extTys[i];
                auto field = TypedValue(elem, ty);
                storeValue(c, field);

                auto size = ty->getSizeInBits(c);
                if(!size){
                    cerr << size.getErr() << endl;
                    return;
                }
                data = (char*)data + size.getVal() / 8;
            }
            data = orig_data;
        }else{
            //single-value "tuple"
            AnType *ty = sty->extTys[0];
            auto field = TypedValue(tup.val, ty);
            storeValue(c, field);
        }
    }

    /**
     * Finds and returns the last stored value from a LoadInst
     * of a mutable variable.
     */
    TypedValue findLastStore(Compiler *c, TypedValue const& tv){
        //mutable pointer passed, find last store
        if(LoadInst *si = dyn_cast<LoadInst>(tv.val)){
            for(auto *u : si->getPointerOperand()->users()){
                if(StoreInst *si = dyn_cast<StoreInst>(u)){
                    Value *vo = si->getValueOperand();

                    if(BitCastInst *be = dyn_cast<BitCastInst>(vo)){
                        for(auto *u : be->users()){
                            if(StoreInst *si = dyn_cast<StoreInst>(u)){
                                return {si->getValueOperand(), tv.type};
                            }
                        }
                    }
                    return {vo, tv.type};
                }
            }
        }

        cerr << "Cannot find last store to mutable variable during translation." << endl;
        tv.dump();
        return {};
    }


    void ArgTuple::storeValue(Compiler *c, TypedValue const& tv){
        auto *ci = dyn_cast<ConstantInt>(tv.val);
        auto *cf = dyn_cast<ConstantFP>(tv.val);

        TypeTag tt = tv.type->typeTag;
        switch(tt){
            case TT_I8:   *(uint8_t*) data = ci->getSExtValue(); return;
            case TT_U8:
            case TT_C8:
            case TT_Bool: *(uint8_t*) data = ci->getZExtValue(); return;
            case TT_I16:  *(uint16_t*)data = ci->getSExtValue(); return;
            case TT_U16:  *(uint16_t*)data = ci->getZExtValue(); return;
            case TT_I32:  *(uint32_t*)data = ci->getSExtValue(); return;
            case TT_U32:  *(uint32_t*)data = ci->getZExtValue(); return;
            case TT_C32:  *(uint32_t*)data = ci->getZExtValue(); return;
            case TT_I64:  *(uint64_t*)data = ci->getSExtValue(); return;
            case TT_U64:  *(uint64_t*)data = ci->getZExtValue(); return;
            case TT_Isz:  *(size_t*)  data = ci->getSExtValue(); return;
            case TT_Usz:  *(size_t*)  data = ci->getZExtValue(); return;
            case TT_F16:  *(float*)   data = cf->getValueAPF().convertToFloat(); return;
            case TT_F32:  *(float*)   data = cf->getValueAPF().convertToFloat(); return;
            case TT_F64:  *(double*)  data = cf->getValueAPF().convertToDouble(); return;
            case TT_Ptr:
            case TT_Array: storePtr(c, tv); return;
            case TT_Tuple: storeTuple(c, tv); return;
            case TT_TypeVar: {
                auto *tvt = (AnTypeVarType*)tv.type;
                auto *var = c->lookup(tvt->name);
                if(!var){
                    cerr << AN_ERR_COLOR << "error: " << AN_CONSOLE_RESET << "Lookup for typevar "+tvt->name+" failed";
                    c->errFlag = true;
                    return;
                }

                auto *type = extractTypeValue(var->tval);
                auto boundTv = TypedValue(tv.val, type);
                storeValue(c, boundTv);
                return;
            }
            case TT_Data: storeTuple(c, tv); return;
            case TT_Function:
            case TT_TaggedUnion:
            case TT_MetaFunction:
            case TT_FunctionList:
            case TT_Type:
            case TT_Void:
                break;
        }

        cerr << AN_ERR_COLOR << "error: " << AN_CONSOLE_RESET << "Compile-time function argument must be constant.\n";
        c->errFlag = true;
    }


    /*
    *  Converts a TypedValue to an llvm GenericValue
    *  - Assumes the Value* within the TypedValue is a Constant*
    */
    ArgTuple::ArgTuple(Compiler *c, vector<TypedValue> const& tvals)
            : data(nullptr){

        size_t size = 0;
        for(auto &tv : tvals){
            auto elemSize = tv.type->getSizeInBits(c);
            if(!elemSize){
                cerr << elemSize.getErr() << endl;
                return;
            }

            elemSize = elemSize.getVal() / 8;

            //we're reallocating the data manually here for the tuple
            //so storeValue must be used instead of allocAndStore
            void *dataBegin = realloc(data, size + elemSize.getVal());
            data = (char*)dataBegin + size;

            if(tv.type->hasModifier(Tok_Mut)){
                storeValue(c, findLastStore(c, tv));
            }else{
                storeValue(c, tv);
            }

            data = dataBegin;
            size += elemSize.getVal();
        }
    }


    ArgTuple::ArgTuple(Compiler *c, TypedValue const& val)
            : data(nullptr), tval(val){

        if(val.type->hasModifier(Tok_Mut)){
            allocAndStoreValue(c, findLastStore(c, val));
        }else{
            allocAndStoreValue(c, val);
        }
    }


    /**
     * Constructs an ArgTuple using the given pre-initialized data.
     */
    ArgTuple::ArgTuple(Compiler *c, void *d, AnType *t) : data(d){
        this->tval = convertToTypedValue(c, *this, t);
    }
}
