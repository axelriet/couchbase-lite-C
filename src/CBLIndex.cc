//
// CBLIndex.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "CBLIndex_Internal.hh"
#include "CBLBlob_Internal.hh"
#include "CBLCollection_Internal.hh"
#include "c4Index.hh"

using namespace fleece;

#pragma mark - CBLIndex:

CBLIndex::CBLIndex(Retained<C4Index>&& index, CBLCollection* collection)
:_collection(collection)
,_c4Index(std::move(index), _collection->database()->c4db().get())
{ }

CBLCollection* CBLIndex::collection() const {
    return _collection;
}

slice CBLIndex::name() const {
    return _c4Index.useLocked()->getName();
}

#ifdef COUCHBASE_ENTERPRISE

Retained<CBLIndexUpdater> CBLIndex::beginUpdate(size_t limit) {
    auto updater = _c4Index.useLocked()->beginUpdate(limit);
    if (!updater) {
        return nullptr;
    }
    return new CBLIndexUpdater(std::move(updater), this);
}

#pragma mark - CBLIndexUpdater:

static const char* kCBLIndexUpdaterName = "CBLIndexUpdater";

CBLIndexUpdater::CBLIndexUpdater(Retained<C4IndexUpdater>&& indexUpdater, CBLIndex* index)
:_c4IndexUpdater(indexUpdater)
,_c4IndexUpdaterWithLock(std::move(indexUpdater), index->collection()->database()->c4db().get())
,_index(index)
{  }

CBLIndexUpdater::~CBLIndexUpdater() {
    if (_fleeceDoc) {
        _fleeceDoc.setAssociated(nullptr, kCBLIndexUpdaterName);
    }
}

size_t CBLIndexUpdater::count() const {
    LOCK(_mutex);
    return _c4IndexUpdater->count();
}

FLValue _cbl_nullable CBLIndexUpdater::value(size_t index) {
    LOCK(_mutex);
    auto val = _c4IndexUpdater->valueAt(index);
    
    // Associate myself with the `Doc` backing the Fleece
    // data, so that the `getBlob()` method can find me.
    if (!_fleeceDoc && val) {
        _fleeceDoc = Doc::containing(val);
        if (!_fleeceDoc.setAssociated(this, kCBLIndexUpdaterName))
            C4Warn("Couldn't associate CBLIndexUpdater with FLDoc %p", FLDoc(_fleeceDoc));
    }
    
    return val;
}

void CBLIndexUpdater::setVector(size_t index, const float* _cbl_nullable vector, size_t dimension) {
    LOCK(_mutex);
    _c4IndexUpdater->setVectorAt(index, vector, dimension);
}

void CBLIndexUpdater::skipVector(size_t index) {
    LOCK(_mutex);
    _c4IndexUpdater->skipVectorAt(index);
}

void CBLIndexUpdater::finish() {
    LOCK(_mutex);
    _c4IndexUpdaterWithLock.useLocked()->finish(); // Ignore return value
}

Retained<CBLIndexUpdater> CBLIndexUpdater::containing(Value v) {
    return (CBLIndexUpdater*) Doc::containing(v).associated(kCBLIndexUpdaterName);
}

CBLBlob* CBLIndexUpdater::getBlob(Dict blobDict, const C4BlobKey &key) {
    // OK, let's find or create a CBLBlob, then cache it.
    // (It's not really necessary to cache the CBLBlobs -- they're lightweight objects --
    // but otherwise we'd have to return a `Retained<CBLBlob>`, which would complicate
    // the public C API by making the caller release it afterwards.)
    LOCK(_mutex);
    auto i = _blobs.find(blobDict);
    if (i == _blobs.end()) {
        auto db = const_cast<CBLDatabase*>(_index->collection()->database());
        i = _blobs.emplace(blobDict, new CBLBlob(db, blobDict, key)).first;
    }
    return i->second;
}

#endif
