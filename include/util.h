#ifndef AN_UTIL_H
#define AN_UTIL_H

#include "error.h"
#include "compiler.h"
#include "types.h"
#include "types.h"
#include <memory>
#include <llvm/ADT/StringMap.h>

namespace ante {
    template<typename F>
    void tryTo(F f){
        try{
            f();
        }catch(CtError){}
    }

#   define TRY_TO(f) ante::tryTo([&](){ f; })

    //Define a new assert macro so it remains in the binary even if NDEBUG is defined.
    //Implement on one line to keep __LINE__ referring to the correct assertion line.
    #define ASSERT_UNREACHABLE(msg) { fprintf(stderr, "Internal compiler error: " msg "\nassert_unreachable failed on line %d of file '%s'\n", __LINE__, \
                                        __FILE__); exit(1); }

    bool showTimingInformation();

    /** @brief Create a vector with a capacity of at least cap elements. */
    template<typename T> std::vector<T> vecOf(size_t cap){
        std::vector<T> vec;
        vec.reserve(cap);
        return vec;
    }

    /** Perform an element count in linear time for lists or similar data structures. */
    template<typename T>
    size_t count(T& collection){
        size_t i = 0;
        for(auto it = collection.begin(), end = collection.end(); it != end; ++it){
            ++i;
        }
        return i;
    }

    /** Given [a, b, ..., z], f: return [f(a), f(b), ..., f(z)] */
    template<typename T, typename F,
        typename U = typename std::decay<typename std::result_of<F&(typename std::vector<T>::const_reference)>::type>::type>
    std::vector<U> applyToAll(std::vector<T> const& vec, F f){
        std::vector<U> result;
        result.reserve(vec.size());
        for(const auto& elem : vec){
            result.emplace_back(f(elem));
        }
        return result;
    }

    /**
     * Similar to applyToAll, but works on any iterable object and
     * as a result cannot efficiently reserve() the resulting vector.
     */
    template<typename T, typename F,
        typename U = typename std::result_of<F&(typename T::const_reference)>::type>
    std::vector<U> collect(T const& iterable, F f){
        std::vector<U> result;
        for(const auto& elem : iterable){
            result.push_back(f(elem));
        }
        return result;
    }

    template<typename T, typename E>
    typename T::const_iterator find(T const& collection, E const& elem){
        auto it = collection.cbegin();
        auto end = collection.cend();
        for(; it != end; ++it){
            if(*it == elem){
                return it;
            }
        }
        return end;
    }

    template<typename T, typename F>
    typename T::const_iterator find_if(T const& collection, F fn){
        auto it = collection.cbegin();
        auto end = collection.cend();
        for(; it != end; ++it){
            if(fn(*it)){
                return it;
            }
        }
        return end;
    }

    template<typename T, typename F>
    bool remove_if(T& collection, F fn){
        auto it = collection.begin();
        auto end = collection.end();
        for(; it != end; ++it){
            if(fn(*it)){
                collection.erase(it);
                return true;
            }
        }
        return false;
    }

    template<typename E, typename T>
    bool in(E const& elem, T const& collection){
        return ante::find(collection, elem) != collection.cend();
    }

    template<typename F, typename T>
    bool any(T const& collection, F fn){
        return ante::find_if(collection, fn) != collection.cend();
    }

    template<typename F, typename T, typename E>
    bool all(T const& collection, F fn){
        return !ante::any(collection, [fn](E x){ return !fn(x); });
    }

    std::ostream& operator<<(std::ostream &o, AnType *t);

    template<typename A, typename B>
    std::ostream& operator<<(std::ostream &o, std::pair<A, B> const& p){
        return o << '(' << p.first << ", " << p.second << ')';
    }

    template<typename T>
    std::ostream& operator<<(std::ostream &o, std::vector<T> const& vec){
        o << '[';
        for(auto &elem : vec){
            o << elem;
            if(&elem != &vec.back())
                o << ", ";
        }
        return o << ']';
    }

    template<typename T>
    std::ostream& operator<<(std::ostream &o, std::list<T> const& l){
        o << '[';
        auto it = l.begin(), end = l.end();
        while(it != end){
            o << *it;
            ++it;
            if(it != end)
                o << ", ";
        }
        return o << ']';
    }

    template<typename T>
    std::ostream& operator<<(std::ostream &o, llvm::StringMap<T> const& map){
        o << '[';
        for(auto &pair : map){
            o << pair.first().str() << " -> " << pair.second << ", ";
        }
        return o << ']';
    }

    /** @return n == 1 ? "" : pluralSuffix */
    std::string plural(int n, std::string pluralSuffix = "s");

    /** @return n == 1 ? "is" : "are" */
    std::string pluralIsAre(int n);

    void show(parser::Node *n);
    void show(std::shared_ptr<parser::Node> const& n);
    void show(std::unique_ptr<parser::Node> const& n);
    std::ostream& operator<<(std::ostream &out, parser::Node &n);
    std::ostream& operator<<(std::ostream &out, AnType &n);
}

#endif
