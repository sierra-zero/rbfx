//
// Copyright (c) 2008-2020 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include <cassert>

#include <EASTL/internal/thread_support.h>

#include "../Container/RefCounted.h"
#include "../Core/Macros.h"
#if URHO3D_CSHARP
#   include "../Script/Script.h"
#endif

namespace Urho3D
{

RefCount* RefCount::Allocate()
{
    void* const memory = EASTLAlloc(*ea::get_default_allocator((Allocator*)nullptr), sizeof(RefCount));
    assert(memory != nullptr);
    return ::new(memory) RefCount();
}

void RefCount::Free(RefCount* instance)
{
    instance->~RefCount();
    EASTLFree(*ea::get_default_allocator((Allocator*)nullptr), instance, sizeof(RefCount));
}

RefCounted::RefCounted()
    : refCount_(RefCount::Allocate())
{
    // Hold a weak ref to self to avoid possible double delete of the refcount
    refCount_->weakRefs_++;
}

RefCounted::~RefCounted()
{
    assert(refCount_);
    assert(refCount_->refs_ == 0);
    assert(refCount_->weakRefs_ > 0);

#if URHO3D_CSHARP
    // Dispose of managed object when native object was a part of other object (did not use refcounting). Native
    // destructors have run their course already. This is fine, because Dispose(true) is called only for wrapped but not
    // inherited classes and Dispose(true) of wrapper classes merely sets C++ pointer to null and does not interact with
    // native object in any way.
    if (scriptObject_)
    {
        // API may be null when application when finalizers run on application exit.
        ScriptRuntimeApi* api = Script::GetRuntimeApi();
        assert(api != nullptr);
        api->Dispose(this);
        assert(scriptObject_ == nullptr);
    }
#endif

    // Mark object as expired, release the self weak ref and delete the refcount if no other weak refs exist
    refCount_->refs_ = -1;

    if (ea::Internal::atomic_decrement(&refCount_->weakRefs_) == 0)
        RefCount::Free(refCount_);

    refCount_ = nullptr;
}

int RefCounted::AddRef()
{
    int refs = ea::Internal::atomic_increment(&refCount_->refs_);
    assert(refs > 0);
#if URHO3D_CSHARP
    if (URHO3D_UNLIKELY(scriptObject_ && /*!isScriptStrongRef_ &&*/ refs - refCount_->scriptRefs_ == 1))
    {
        // More than one native reference exists. Ensure strong GC handle to prevent garbage collection of managed
        // wrapper object.
        ScriptRuntimeApi* api = Script::GetRuntimeApi();
        assert(api != nullptr);
#if URHO3D_DEBUG
        assert(!isScriptStrongRef_);
        isScriptStrongRef_ = true;
#endif
        scriptObject_ = api->RecreateGCHandle(scriptObject_, true);
    }
#endif
    return refs;
}

int RefCounted::ReleaseRef()
{
    int refs = ea::Internal::atomic_decrement(&refCount_->refs_);
    assert(refs >= 0);
#if URHO3D_CSHARP
    if (refs == 0)
    {
        // Dispose managed object while native object is still intact. This code path is taken for user classes that
        // inherit from a native base, because such objects are always heap-allocated. Because of this, it is guaranteed
        // that user's Dispose(true) method will be called before execution of native object destructors.
        if (scriptObject_)
        {
            // API may be null when application when finalizers run on application exit.
            ScriptRuntimeApi* api = Script::GetRuntimeApi();
            assert(api != nullptr);
            api->Dispose(this);
            assert(scriptObject_ == nullptr);
        }
        delete this;
    }
    else if (URHO3D_UNLIKELY(scriptObject_ && refs == refCount_->scriptRefs_))
    {
        // No more native references exist. Convert GC handle to weak. If user loses all managed references such
        // object can be freed by GC.
        auto api = Script::GetRuntimeApi();
        assert(api != nullptr);
#if URHO3D_DEBUG
        assert(isScriptStrongRef_);
        isScriptStrongRef_ = false;
#endif
        scriptObject_ = api->RecreateGCHandle(scriptObject_, false);
    }
#else
    if (refs == 0)
        delete this;
#endif
    return refs;
}
#if URHO3D_CSHARP
int RefCounted::ScriptAddRef()
{
    // Adding a script reference does not affect GC handle type.
    int refs = ea::Internal::atomic_increment(&refCount_->refs_);
    int scriptRefs = ea::Internal::atomic_increment(&refCount_->scriptRefs_);
    assert(refs > 0);
    assert(scriptRefs > 0);
    assert(scriptRefs <= refs);
    return scriptRefs;
}

int RefCounted::ScriptReleaseRef()
{
    int scriptRefs = ea::Internal::atomic_decrement(&refCount_->scriptRefs_);
    int refs = ea::Internal::atomic_decrement(&refCount_->refs_);
    assert(refs >= 0);
    assert(scriptRefs >= 0);
    assert(scriptRefs <= refs);
    if (URHO3D_UNLIKELY(refs == 0))
    {
        // Dispose managed object while native object is still intact. This code path is taken for user classes that
        // inherit from a native base, because such objects are always heap-allocated. Because of this, it is guaranteed
        // that user's Dispose(true) method will be called before execution of native object destructors.
        if (scriptObject_)
        {
            // API may be null when application when finalizers run on application exit.
            ScriptRuntimeApi* api = Script::GetRuntimeApi();
            assert(api != nullptr);
            api->Dispose(this);
            assert(scriptObject_ == nullptr);
        }
        delete this;
    }
    return scriptRefs;
}
#endif
int RefCounted::Refs() const
{
    return refCount_->refs_;
}

int RefCounted::WeakRefs() const
{
    // Subtract one to not return the internally held reference
    return refCount_->weakRefs_ - 1;
}
#if URHO3D_CSHARP
int RefCounted::ScriptRefs() const
{
    return refCount_->scriptRefs_;
}

void RefCounted::SetScriptObject(void* handle, bool isStrong)
{
    if (scriptObject_ != nullptr)
    {
        auto api = Script::GetRuntimeApi();
        assert(api != nullptr);
        api->FreeGCHandle(scriptObject_);
    }
    scriptObject_ = handle;
#if URHO3D_DEBUG
    isScriptStrongRef_ = isStrong;
#endif
}

void RefCounted::ResetScriptObject()
{
    scriptObject_ = nullptr;
#if URHO3D_DEBUG
    isScriptStrongRef_ = false;
#endif
}
#endif
}
