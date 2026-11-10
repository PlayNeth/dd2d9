// What the surface files (surface.cpp, attach.cpp, lock.cpp, buffer.cpp, palette.cpp,
// clipper.cpp) share.
#pragma once
#include "objects.h"

#include <algorithm>
#include <vector>

namespace dd2d9 {

// The live objects of one kind (palettes, clippers; g_lock): ddraw.dll checks that their
// methods are called on a live one. The list is never freed: a game may still call
// Release as the process ends.
template <class T>
class LiveSet {
public:
    void Add(const T* o) {
        Guard l;
        if (!list_) list_ = new std::vector<const T*>;
        list_->push_back(o);
        vtbl_ = *reinterpret_cast<void* const*>(o);
    }
    void Remove(const T* o) { list_->erase(std::find(list_->begin(), list_->end(), o)); }  // g_lock held
    bool Has(const T* o) const {
        Guard l;
        return list_ && std::find(list_->begin(), list_->end(), o) != list_->end();
    }
    // Its vtable is still the genuine one (as ddraw.dll checks it in Release).
    bool Genuine(const T* o) const { return *reinterpret_cast<void* const*>(o) == vtbl_; }

private:
    std::vector<const T*>* list_ = nullptr;
    const void* vtbl_ = nullptr;
};

bool LivePalette(const Palette* p);  // palette.cpp
bool LiveClipper(const Clipper* c);  // clipper.cpp

}  // namespace dd2d9
