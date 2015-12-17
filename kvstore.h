//
// Created by chris on 10/7/15.
//

#ifndef FLEXIS_FLEXIS_KVSTORE_H
#define FLEXIS_FLEXIS_KVSTORE_H

#include <string>
#include <typeinfo>
#include <typeindex>
#include <memory>
#include <functional>
#include <unordered_map>
#include <persistence_error.h>
#include <FlexisPersistence_Export.h>
#include <type_traits>

#include "kvtraits.h"

#define PROPERTY_ID(cls, name) flexis::persistence::kv::ClassTraits<cls>::PropertyIds::name
#define PROPERTY(cls, name) flexis::persistence::kv::ClassTraits<cls>::decl_props[flexis::persistence::kv::ClassTraits<cls>::PropertyIds::name-1]

#define IS_SAME(cls, var, prop, other) flexis::persistence::kv::ClassTraits<cls>::same(\
  var, flexis::persistence::kv::ClassTraits<cls>::PropertyIds::prop, other)

namespace flexis {
namespace persistence {

using namespace kv;

static const ClassId COLLECTION_CLSID = 1;
static const ClassId COLLINFO_CLSID = 2;
static const size_t CHUNKSIZE = 1024 * 2; //default chunksize. All data in one page

class incompatible_schema_error : public persistence_error
{
public:
  incompatible_schema_error(std::string detail)
      : persistence_error("database is not compatible with current class schema", detail) {}
};

class class_not_registered_error : public persistence_error
{
public:
  class_not_registered_error(const std::string className)
      : persistence_error("class has not been registered", className) {}
};

namespace kv {
class ReadTransaction;
class ExclusiveReadTransaction;
class WriteTransaction;
}

class FlexisPersistence_EXPORT KeyValueStoreBase
{
  friend class kv::ReadTransaction;
  friend class kv::WriteTransaction;

protected:
  virtual ~KeyValueStoreBase() {}

  //property info stored in database
  struct PropertyMetaInfo {
    std::string name;
    PropertyId id;
    unsigned typeId;
    bool isVector;
    unsigned byteSize;
    std::string className;
  };
  using PropertyMetaInfoPtr = std::shared_ptr<PropertyMetaInfo>;

  /**
   * check if class schema already exists. If so, check compatibility. If not, create
   * @throws incompatible_schema_error
   */
  void updateClassSchema(AbstractClassInfo *classInfo, PropertyAccessBase * properties[], unsigned numProperties);

  /**
   * load class metadata from the store. If it doesn't already exist, save currentProps as metadata
   *
   * @param (in/out) the ClassInfo which holds the fully qualified class name. The other fields will
   * be set
   * @param (in) currentProps the currently live persistent properties
   * @param (in) numProps the length of the above array
   * @param (out) the persistent propertyInfos. This will be empty if the class was newly declared
   */
  virtual void loadSaveClassMeta(
      AbstractClassInfo *classInfo,
      PropertyAccessBase * currentProps[],
      unsigned numProps,
      std::vector<PropertyMetaInfoPtr> &propertyInfos) = 0;
};

using ReadTransactionPtr = std::shared_ptr<kv::ReadTransaction>;
using ExclusiveReadTransactionPtr = std::shared_ptr<kv::ExclusiveReadTransaction>;
using WriteTransactionPtr = std::shared_ptr<kv::WriteTransaction>;

using ObjectProperties = std::unordered_map<ClassId, Properties *>;
using ObjectClassInfos = std::unordered_map<ClassId, AbstractClassInfo *>;

using TypeInfoRef = std::reference_wrapper<const std::type_info>;

struct TypeinfoHasher {
  std::size_t operator()(TypeInfoRef code) const
  {
    return code.get().hash_code();
  }
};

struct TypeinfoEqualTo {
  bool operator()(TypeInfoRef lhs, TypeInfoRef rhs) const
  {
    return lhs.get() == rhs.get();
  }
};

/**
 * high-performance key/value store interface. Most application-relevant functions are provided by ReadTransaction
 * and WriteTransaction, which can be obtined from this class
 */
class FlexisPersistence_EXPORT KeyValueStore : public KeyValueStoreBase
{
  friend class kv::ReadTransaction;
  friend class kv::ExclusiveReadTransaction;
  friend class kv::WriteTransaction;

  ClassId minAbstractClassId = UINT32_MAX;

  //backward mapping from ClassId, used during polymorphic operations
  ObjectProperties objectProperties;
  ObjectClassInfos objectClassInfos;

  std::unordered_map<TypeInfoRef, ClassId, TypeinfoHasher, TypeinfoEqualTo> typeInfos;

protected:
  ClassId m_maxClassId = 0;
  ObjectId m_maxCollectionId = 0;

public:
  /**
   * register a type for key/value persistence. It is assumed that a ClassTraits<type> implementation is visibly defined in the
   * current namespace. If this is the first call for this type, a ClassId and a ObjectId generator will be persistently
   * allocated.
   * Since this call determines the persistence mapping, care must be taken in case of class changes to ensure downward
   * compatibility for already stored class instance data
   *
   * @param ignoreIfUnknown if true, ignore unknown subtypes (i.e., don't subsitute by T). This flag will implicitly be
   * true if T is abstract
   */
  template <typename T>
  void registerType(bool ignoreIfUnknown=false)
  {
    using Traits = ClassTraits<T>;

    if(Traits::isAbstract) ignoreIfUnknown = true;

    updateClassSchema(Traits::info, Traits::decl_props, Traits::num_decl_props);

    //establish inheritance chain
    Traits::info->publish();

    //make sure all propertyaccessors have correct classId
    for(int i=0; i<Traits::num_decl_props; i++)
      Traits::decl_props[i]->classId = Traits::info->classId;

    objectProperties[Traits::info->classId] = Traits::properties;
    objectClassInfos[Traits::info->classId] = Traits::info;

    const std::type_info &ti = typeid(T);
    typeInfos[ti] = Traits::info->classId;
  }

  /**
   * register a substitype type to be used in polymorphic operations where a subclass of T is unknown (accessing
   * incompatible schema). Subst must me a non-KV mapped subclass of T
   */
  template <typename T, typename Subst>
  void registerSubstitute()
  {
    ClassTraits<T>::info-> template setSubstitute<Subst>();
  }

  template <typename T> ObjectId getObjectId(std::shared_ptr<T> &obj)
  {
    return ClassTraits<T>::getObjectId(obj);
  }

  template <typename T> bool isNew(std::shared_ptr<T> &obj)
  {
    return ClassTraits<T>::getObjectId(obj) == 0;
  }

  /**
   * @return a transaction object that allows reading the database.
   */
  virtual ReadTransactionPtr beginRead() = 0;

  /**
   * @return a transaction object that allows reading the database but prevents writing
   */
  virtual ExclusiveReadTransactionPtr beginExclusiveRead() = 0;

  /**
   * @param append enable append mode. Append mode, if supported, is useful if a large number of homogenous simple objects
   * are written, homogenous meaning that objects are of the same type (or subtypes). One essential requirement is that keys
   * are written in sequential order (hence append), which is maintained if only the putObject API is used. However, this
   * cannot be ensured if the objects written are complex, i.e. contain references to other mapped objects (either direct or
   * as array elements), which is why these objects are not allowed.
   * Writing in append mode can be much more efficient than standard write
   *
   * @param needsKBs database space required by this transaction. If not set, the default will be used.
   *
   * @return a transaction object that allows reading + writing the database.
   * @throws InvalidArgumentException if in append mode the above prerequisites are not met
   * @throws persistence_error if write operations are currently blocked (beginRead(true))
   */
  virtual WriteTransactionPtr beginWrite(bool append=false, unsigned needsKBs=0) = 0;
};

namespace kv {

void readChunkHeader(const byte_t *data, size_t *dataSize, size_t *startIndex, size_t *elementCount);
void readChunkHeader(ReadBuf &buf, size_t *dataSize, size_t *startIndex, size_t *elementCount);
void readObjectHeader(ReadBuf &buf, ClassId *classId, ObjectId *objectId, size_t *size=nullptr, bool *deleted=nullptr);

/**
 * instantiate an object and read object data polymorphically
 */
template<typename T> T *readObject(ReadTransaction *tr, ReadBuf &buf,
                                   ClassId classId, ObjectId objectId, ClassInfo<T> *info = nullptr)
{
  T *obj = info ? info->makeObject(classId) : ClassTraits<T>::makeObject(classId);
  Properties *props = ClassTraits<T>::getProperties(classId);
  if(!props) throw persistence_error("unknown classId. Class not registered");

  PropertyId propertyId = 0;
  for(unsigned px=0, sz=props->full_size(); px < sz; px++) {
    //we use the key index (+1) as id
    propertyId++;

    PropertyAccessBase *p = props->get(px);
    if(!p->enabled) continue;

    ClassTraits<T>::load(tr, buf, classId, objectId, obj, p);
  }
  return obj;
}

/**
 * read object data non-polymorphically
 */
template<typename T> void readObject(ReadTransaction *tr, ReadBuf &buf, T &obj,
                                     ClassId classId, ObjectId objectId, StoreMode mode = StoreMode::force_none)
{
  Properties *props = ClassTraits<T>::properties;

  PropertyId propertyId = 0;
  for(unsigned px=0, sz=props->full_size(); px < sz; px++) {
    //we use the key index (+1) as id
    propertyId++;

    PropertyAccessBase *p = props->get(px);
    if(!p->enabled) continue;

    ClassTraits<T>::load(tr, buf, classId, objectId, &obj, p, mode);
  }
}

template <typename T> class IterPropertyBackend
{
protected:
  ObjectId m_collectionId = 0;
  KeyValueStore *m_store;

public:
  ObjectId getCollectionId() {return m_collectionId;}
  void setCollectionId(ObjectId collectionId) {m_collectionId = collectionId;}
  void setKVStore(KeyValueStore *store) {m_store = store;}
};
template <typename T> using IterPropertyBackendPtr = std::shared_ptr<IterPropertyBackend<T>>;

/**
 * Helper interface used by cursor, to be extended by implementors
 */
class FlexisPersistence_EXPORT CursorHelper {
  template <typename T> friend class ClassCursor;

protected:
  ClassId m_currentClassId;
  ObjectId m_currentObjectId;

  virtual ~CursorHelper() {}

  /**
   * position the cursor at the first object of the given class.
   * @return true if an object was found
   */
  virtual bool start() = 0;

  /**
   * position the cursor at the next object.
   * @return true if an object was found
   */
  virtual bool next() = 0;

  /**
   * delete the object at the current cursor position. Cursor is not moved
   */
  virtual void erase() = 0;

  /**
   * @return the objectId of the item at the current cursor position
   */
  ObjectId currentObjectId() {return m_currentObjectId;}

  /**
   * @return the classId of the item at the current cursor position
   */
  ClassId currentClassId() {return m_currentClassId;}

  /**
   * close the cursor an release all resources
   */
  virtual void close() = 0;

  /**
   * read the data at the current cursor position into the key and buffer
   */
  virtual void get(StorageKey &key, ReadBuf &rb) = 0;

  /**
   * @return the data at the current cursor position
   */
  virtual const byte_t *getObjectData() = 0;
};

struct ChunkInfo {
  PropertyId chunkId = 0;
  size_t startIndex = 0;
  size_t elementCount = 0;
  size_t dataSize = 0;

  ChunkInfo() {}
  ChunkInfo(PropertyId chunkId, size_t startIndex, size_t elementCount=0, size_t dataSize=0)
      : chunkId(chunkId), startIndex(startIndex), elementCount(elementCount), dataSize(dataSize) {}
  ChunkInfo(PropertyId chunkId) : chunkId(chunkId) {}

  bool operator == (const ChunkInfo &other) {
    return chunkId == other.chunkId;
  }
  bool operator <= (const ChunkInfo &other) {
    return chunkId <= other.chunkId;
  }
};
struct CollectionInfo
{
  //unique collection id
  ObjectId collectionId = 0;

  //collection chunks
  std::vector <ChunkInfo> chunkInfos;

  PropertyId nextChunkId = 1;
  size_t nextStartIndex = 0;

  CollectionInfo() {}
  CollectionInfo(ObjectId collectionId) : collectionId(collectionId) {}
};

class ChunkCursor
{
protected:
  bool m_atEnd;

public:
  using Ptr = std::shared_ptr<ChunkCursor>;

  bool atEnd() const {return m_atEnd;}
  virtual bool next() = 0;
  virtual void get(ReadBuf &rb) = 0;
  virtual PropertyId chunkId() = 0;
  virtual void close() = 0;
};

/**
 * top-level collection cursor
 */
class CollectionCursorBase
{
protected:
  ChunkCursor::Ptr m_chunkCursor;
  ReadTransaction * const m_tr;
  const ObjectId m_collectionId;

  ReadBuf m_readBuf;
  size_t m_elementCount = 0, m_curElement = 0;
  virtual bool isValid() {return true;}
public:
  CollectionCursorBase(ObjectId collectionId, ReadTransaction *tr, ChunkCursor::Ptr chunkCursor);
  bool atEnd();
  bool next();
};

/**
 * cursor for iterating over top-level object collections
 */
template <typename T>
class ObjectCollectionCursor : public CollectionCursorBase
{
  const ClassId m_declClass;

protected:
  bool isValid() override
  {
    return !read_integer<byte_t>(m_readBuf.data() + ObjectHeader_sz - 1, 1);
  }

public:
  using Ptr = std::shared_ptr<ObjectCollectionCursor<T>>;

  ObjectCollectionCursor(ObjectId collectionId, ReadTransaction *tr, ChunkCursor::Ptr chunkCursor)
      : CollectionCursorBase(collectionId, tr, chunkCursor), m_declClass(ClassTraits<T>::info->classId)
  {}

  T *get()
  {
    ClassId classId;
    ObjectId objectId;
    readObjectHeader(m_readBuf, &classId, &objectId);

    T *obj = (T *)ClassTraits<T>::makeObject(classId);
    Properties *properties = ClassTraits<T>::getProperties(classId);

    PropertyId propertyId = 0;
    for(unsigned px=0, sz=properties->full_size(); px < sz; px++) {
      //we use the key index (+1) as id
      propertyId++;

      PropertyAccessBase *p = properties->get(px);
      if(!p->enabled) continue;

      ClassTraits<T>::load(m_tr, m_readBuf, m_declClass, objectId, obj, p);
    }
    return obj;
  }
};

/**
 * cursor for iterating over top-level object collections
 */
template <typename T>
class ValueCollectionCursor : public CollectionCursorBase
{
public:
  using Ptr = std::shared_ptr<ValueCollectionCursor<T>>;

  ValueCollectionCursor(ObjectId collectionId, ReadTransaction *tr, ChunkCursor::Ptr chunkCursor)
      : CollectionCursorBase(collectionId, tr, chunkCursor)
  {}

  T get()
  {
    T val;
    ValueTraits<T>::getBytes(m_readBuf, val);
    return val;
  }
};

/**
 * cursor for iterating over class objects (each with its own key)
 */
template <typename T>
class ClassCursor
{
  ClassCursor(ClassCursor<T> &other) = delete;

  CursorHelper * const m_helper;
  ReadTransaction * const m_tr;
  bool m_hasData;
  ClassInfo<T> *m_classInfo;

  bool validateClass() {
    m_classInfo = FIND_CLS(T, m_helper->currentClassId());
    return m_classInfo != nullptr || ClassTraits<T>::info->substitute != nullptr;
  }

public:
  using Ptr = std::shared_ptr<ClassCursor<T>>;

  ClassCursor(CursorHelper *helper, ReadTransaction *tr) : m_helper(helper), m_tr(tr)
  {
    bool hasData = helper->start();
    bool clsFound = validateClass();

    while(hasData && !clsFound) {
      hasData = helper->next();
      clsFound = hasData && validateClass();
    }
    m_hasData = hasData && clsFound;
  }

  virtual ~ClassCursor() {
    delete m_helper;
  }

  void erase()
  {
    m_helper->erase();
  }

  /**
   * retrieve the address of the value of the given object property at the current cursor position. Note that
   * the address may point to database-owned memory and therefore must not be written to. It may also become
   * invalid after the end of the transaction.
   *
   * @param propertyId the property ID (1-based index into declared properties)
   * @param data (out) pointer to the property value. Note that the pointer will be invalid for Object pointer and
   * vector properties
   * @param buf (int/out) pointer to the object data buffer. If this pointer is non-null, the address of the object data buffer
   * will be stored there on the first call and reused on subsequent calls
   */
  void get(PropertyAccessBase *p, const byte_t **data, const byte_t **buf=nullptr)
  {
    using Traits = ClassTraits<T>;

    if(!p->enabled || p->type.isVector) {
      *data = nullptr;
      return;
    }

    //load class buffer
    const byte_t *dta;
    if(buf) {
      if(*buf) dta = *buf;
      else *buf = dta = m_helper->getObjectData();
    }
    else dta = m_helper->getObjectData();

    //calculate the buffer offset
    for(unsigned i=0, sz=Traits::properties->full_size(); i<sz; i++) {
      auto prop = Traits::properties->get(i);
      if(prop == p) {
        *data = dta;
        return;
      }
      dta += prop->storage->size(dta);
    }
  }

  /**
   * @param objId (out) the address to store the ObjectId
   * @return the ready instantiated object at the current cursor position
   */
  T *get(ObjectId *objId)
  {
    //load the data buffer
    ReadBuf readBuf;
    StorageKey key;
    m_helper->get(key, readBuf);

    //nothing here
    if(readBuf.null()) return nullptr;

    if(objId) *objId = key.objectId;

    if(m_classInfo)
      return readObject<T>(m_tr, readBuf, key.classId, key.objectId, m_classInfo);
    else {
      T *sp = ClassTraits<T>::getSubstitute();
      readObject<T>(m_tr, readBuf, *sp, key.classId, key.objectId);
      return sp;
    }
  }

  /**
   * @return the ready instantiated object at the current cursor position. The shared_ptr also
   * contains the ObjectId
   */
  std::shared_ptr<T> get()
  {
    ObjectId id;
    T *obj = get(&id);
    return make_ptr(obj, id);
  }

  bool next() {
    bool hasData, clsFound;
    do {
      hasData = m_helper->next();
      clsFound = hasData && validateClass();
    } while(hasData && !clsFound);

    m_hasData = hasData && clsFound;
    return m_hasData;
  }

  bool atEnd() {
    return !m_hasData;
  }

  void close() {
    m_helper->close();
  }
};

/**
 * container for a raw data pointer obtained from a top-level value collection
 */
template <typename V> class CollectionData
{
  V *m_data;
  bool m_owned;

public:
  using Ptr = std::shared_ptr<CollectionData>;

  CollectionData(void *data, bool owned) : m_data((V*)data), m_owned(owned) {}
  ~CollectionData() {
    if(m_owned) free(m_data);
  }
  V *data() {return m_data;}
};

/**
 * Transaction that allows read operations only. Read transactions can be run concurrently
 */
class FlexisPersistence_EXPORT ReadTransaction
{
  template<typename T, typename V> friend class ValueVectorPropertyStorage;
  template<typename T, typename V> friend class ValueSetPropertyStorage;
  template<typename T, typename V> friend class ObjectPropertyStorage;
  template<typename T, typename V> friend class ObjectVectorPropertyStorage;
  template<typename T, typename V> friend class ObjectVectorPropertyStorageEmbedded;
  template<typename T, typename V> friend class ObjectPtrVectorPropertyStorageEmbedded;
  template<typename T, typename V, template<typename> class Ptr> friend class ObjectPtrPropertyStorage;
  template<typename T, typename V> friend class ObjectPtrVectorPropertyStorage;
  template<typename T, typename V, typename KVIter, typename Iter> friend struct ObjectIterPropertyStorage;
  friend class CollectionCursorBase;
  template <typename T> friend class ClassCursor;
  friend class CollectionAppenderBase;

  CollectionInfo *readCollectionInfo(ReadBuf &readBuf);

protected:
  KeyValueStore &store;
  bool m_blockWrites;
  std::unordered_map<ObjectId, CollectionInfo *> m_collectionInfos;

  ReadTransaction(KeyValueStore &store) : store(store) {}

  void setBlockWrites(bool blockWrites) {
    m_blockWrites = blockWrites;
  }

  /**
   * load an object from the KV store polymorphically. Used by collections that store the classId
   *
   * @param classId the actual class id, which may be the id of a subclass of T
   * @param objectId the key generated by a previous call to WriteTransaction::putObject()
   * @return the object pointer, or nullptr if the key is not defined.
   */
  template<typename T> T *loadObject(ClassId classId, ObjectId objectId)
  {
    ReadBuf readBuf;
    getData(readBuf, classId, objectId, 0);

    if(readBuf.null()) return nullptr;

    return readObject<T>(this, readBuf, classId, objectId);
  }

  /**
   * load a substitute object from the KV store. This API is used in cases where the class identified by dataClassId
   * is unknown, and a substitute (from the same inheritance hierarchy) has been defined. The substitute class must
   * be a subclass of T and should not ne KV mapped. Only data relevant for T will be loaded
   *
   * @param subst the substitute object. Must have the same superclass as the missing class
   * @param missingClassId the ID of the missing class for which the data should be loaded
   * @param objectId the object ID
   * @return true if object data was found in and read from the store
   */
  template<typename T> bool loadSubstitute(T &subst, ClassId missingClassId, ObjectId objectId)
  {
    ReadBuf readBuf;
    getData(readBuf, missingClassId, objectId, 0);

    if(readBuf.null()) return false;

    readObject<T>(this, readBuf, subst, missingClassId, objectId);
    return true;
  }

  /**
   * completely load the contents of a chunked collection
   */
  template <typename T, template <typename> class Ptr=std::shared_ptr> std::vector<Ptr<T>> loadChunkedCollection(
      CollectionInfo *ci)
  {
    std::vector<Ptr<T>> result;
    for(ChunkCursor::Ptr cc= _openChunkCursor(COLLECTION_CLSID, ci->collectionId); !cc->atEnd(); cc->next()) {
      ReadBuf buf;
      cc->get(buf);

      size_t elementCount;
      readChunkHeader(buf, 0, 0, &elementCount);

      for(size_t i=0; i < elementCount; i++) {
        ClassId cid;
        ObjectId oid;
        bool deleted;
        readObjectHeader(buf, &cid, &oid, nullptr, &deleted);
        if(!deleted && ClassTraits<T>::info->isInstance(cid)) {
          T *obj = readObject<T>(this, buf, cid, oid);
          if(obj) result.push_back(std::shared_ptr<T>(obj));
          else
            throw persistence_error("collection object not found");
        }
      }
    }
    return result;
  }

  /**
   * read sub-object data into a buffer. Used internally by storage traits
   */
  virtual void getData(ReadBuf &buf, ClassId classId, ObjectId objectId, PropertyId propertyId) = 0;

  virtual CursorHelper * _openCursor(const std::vector<ClassId> &classIds) = 0;
  virtual CursorHelper * _openCursor(ClassId classId, ObjectId objectId, PropertyId propertyId) = 0;
  virtual CursorHelper * _openCursor(ClassId classId, ObjectId collectionId) = 0;

  virtual void doReset() = 0;
  virtual void doRenew() = 0;
  virtual void doAbort() = 0;

  /**
   * @return the highes currently stored property ID for the given values
   */
  virtual PropertyId getMaxPropertyId(ClassId classId, ObjectId objectId) = 0;

  virtual bool getNextChunkInfo(ObjectId collectionId, PropertyId *propertyId, size_t *startindex) = 0;

  /**
   * retrieve info about a top-level collection
   *
   * @param collectionId
   * @return the collection info or nullptr
   */
  CollectionInfo *getCollectionInfo(ObjectId collectionId);

  /**
   * @return a cursor ofer a chunked object (e.g., collection)
   */
  virtual ChunkCursor::Ptr _openChunkCursor(ClassId classId, ObjectId objectId, bool atEnd=false) = 0;

public:
  virtual ~ReadTransaction();

  /**
   * load an object from the KV store using the key generated by a previous call to WriteTransaction::putObject().
   * Non-polymorphical, T must be the exact type of the object. The object is allocated on the heap.
   *
   * @param objectId the key generated by a previous call to WriteTransaction::putObject()
   * @return the object pointer, or nullptr if the key is not defined.
   */
  template<typename T> T *loadObject(ObjectId objectId)
  {
    ClassId classId = ClassTraits<T>::info->classId;

    ReadBuf readBuf;
    getData(readBuf, classId, objectId, 0);

    if(readBuf.null()) return nullptr;

    T *tp = new T();
    readObject<T>(this, readBuf, *tp, classId, objectId);

    return tp;
  }


  /**
   * load an object from the KV store, using the key generated by a previous call to WriteTransaction::putObject()
   * Non-polymorphical, T must be the exact type of the object. The object is allocated on the heap.
   *
   * @return a shared pointer to the object, or an empty shared_ptr if the key does not exist. The shared_ptr contains
   * the ObjectId and can thus be handed to other API that requires a KV-valid shared_ptr
   */
  template<typename T> std::shared_ptr<T> getObject(ObjectId objectId)
  {
    T *t = loadObject<T>(objectId);
    return make_ptr(t, objectId);
  }

  /**
   * reload an object from the KV store, Non-polymorphical, T must be the exact type of the object.
   * The new object is allocated on the heap.
   *
   * @return a shared pointer to the object, or an empty shared_ptr if the object does not exist anymore. The shared_ptr contains
   * the ObjectId and can thus be handed to other API that requires a KV-valid shared_ptr
   */
  template<typename T> std::shared_ptr<T> reloadObject(std::shared_ptr<T> &obj)
  {
    ObjectId oid = ClassTraits<T>::getObjectId(obj);
    return make_ptr(loadObject<T>(oid), oid);
  }

  /**
   * @return a cursor over all instances of the given class
   */
  template <typename T> typename ClassCursor<T>::Ptr openCursor() {
    using Traits = ClassTraits<T>;
    std::vector<ClassId> classIds = Traits::info->allClassIds();

    return typename ClassCursor<T>::Ptr(new ClassCursor<T>(_openCursor(classIds), this));
  }

  /**
   * @param objectId a valid object ID
   * @param propertyId the propertyId (1-based index into declared properties)
   * @return a cursor over the contents of a vector-valued, lazy-loading object property. The cursor will be empty if the
   * given property is not vector-valued
   */
  template <typename T, typename V> typename ClassCursor<V>::Ptr openCursor(ObjectId objectId, PropertyId propertyId) {
    ClassId t_classId = ClassTraits<T>::info->classId;

    return typename ClassCursor<V>::Ptr(new ClassCursor<V>(_openCursor(t_classId, objectId, propertyId), this));
  }

  /**
   * @param obj the object that holds the property
   * @param propertyId the propertyId (1-based index into declared properties)
   * @return a cursor over the contents of a vector-valued, lazy-loading object property. The cursor will be empty if the
   * given property is not vector-valued
   */
  template <typename T, typename V> typename ClassCursor<V>::Ptr openCursor(std::shared_ptr<T> obj, PropertyId propertyId) {
    ClassId cid = ClassTraits<T>::info->classId;
    ObjectId oid = ClassTraits<T>::getObjectId(obj);

    return typename ClassCursor<V>::Ptr(new ClassCursor<V>(_openCursor(cid, oid, propertyId), this));
  }

  /**
   * @param collectionId the id of a top-level object collection
   * @return a cursor over the contents of the collection
   */
  template <typename V> typename ObjectCollectionCursor<V>::Ptr openCursor(ObjectId collectionId) {
    return typename ObjectCollectionCursor<V>::Ptr(
        new ObjectCollectionCursor<V>( collectionId, this, _openChunkCursor(COLLECTION_CLSID, collectionId)));
  }

  /**
   * @param collectionId the id of a top-level collection
   * @return a cursor over the contents of the collection. The cursor is non-polymorphic
   */
  template <typename V> typename ValueCollectionCursor<V>::Ptr openValueCursor(ObjectId collectionId) {
    return typename ValueCollectionCursor<V>::Ptr(
        new ValueCollectionCursor<V>(collectionId, this, _openChunkCursor(COLLECTION_CLSID, collectionId)));
  }

  /**
   * retrieve an attached member collection. Attached mebers are stored under a key that is derived from
   * the object they are attached to. The key is the same as if the member was a property member of the
   * attached-to object, but the property does not exist. Instead, the attached member must be loaded and saved
   * explicitly using the given API
   *
   * @param obj the object this property is attached to
   * @param propertyId a property Id which must not be one of the mapped property's id
   * @vect (out)the contents of the attached collection.
   */
  template <typename T, typename V>
  void getCollection(std::shared_ptr<T> &obj, PropertyId propertyId, std::vector<std::shared_ptr<V>> &vect)
  {
    ClassId objClassId = getClassId(typeid(*obj));
    ObjectId objectId = ClassTraits<T>::getObjectId(obj);

    ReadBuf buf;
    getData(buf, objClassId, objectId, propertyId);
    if(buf.null()) return ;

    size_t elementCount = buf.readInteger<size_t>(4);
    vect.reserve(elementCount);

    for(size_t i=0; i < elementCount; i++) {
      StorageKey sk;
      buf.read(sk);

      if(ClassTraits<V>::info->isInstance(sk.classId)) {
        V *obj = loadObject<V>(sk.classId, sk.objectId);
        if(obj) vect.push_back(make_ptr(obj, sk.objectId));
        else
          throw persistence_error("collection object not found");
      }
    }
  }

  /**
   * load a top-level (chunked) object collection
   *
   * @param collectionId an id returned from a previous #putCollection call
   */
  template <typename T, template <typename> class Ptr=std::shared_ptr> std::vector<Ptr<T>> getCollection(ObjectId collectionId)
  {
    CollectionInfo *ci = getCollectionInfo(collectionId);
    return loadChunkedCollection<T, Ptr>(ci);
  }

  /**
   * load a top-level (chunked) member collection.
   *
   * @param o the object that holds the member
   * @param p pointer to the the member variable
   * @return the collection contents
   */
  template <typename O, typename T, template <typename> class Iter>
  std::vector<std::shared_ptr<T>> getCollection(O &o, std::shared_ptr<Iter<T>> O::*p)
  {
    IterPropertyBackend<T> &ib = dynamic_cast<IterPropertyBackend<T> &>(*(o.*p));

    CollectionInfo *ci = getCollectionInfo(ib.getCollectionId());
    return loadChunkedCollection<T, std::shared_ptr>(ci);
  }

  /**
   * load a top-level (chunked) scalar collection
   *
   * @param collectionId an id returned from a previous #putCollection call
   */
  template <typename T>
  std::vector<T> getValueCollection(ObjectId collectionId)
  {
    std::vector<T> result;
    for(ChunkCursor::Ptr cc= _openChunkCursor(COLLECTION_CLSID, collectionId); !cc->atEnd(); cc->next()) {
      ReadBuf buf;
      cc->get(buf);

      size_t elementCount;
      readChunkHeader(buf, 0, 0, &elementCount);

      for(size_t i=0; i < elementCount; i++) {
        T val;
        ValueTraits<T>::getBytes(buf, val);
        result.push_back(val);
      }
    }
    return result;
  }

  /**
   * load a member variable of the given, already persistent object. This is only useful for members which are configured
   * as lazy (only Object* properties)
   *
   * @param objectId an Id obtained from a previous put
   * @param obj the persistent object
   * @param propertyId the propertyId (1-based index of the declared property). Note that the template type
   * parameter must refer to the (super-)class which declares the member
   */
  template <typename T>
  void loadMember(ObjectId objId, T &obj, PropertyAccessBase *pa)
  {
    using Traits = ClassTraits<T>;

    ReadBuf rb;
    ClassTraits<T>::load(this, rb, Traits::info->classId, objId, &obj, pa, StoreMode::force_all);
  }

  /**
   * load a member variable of the given, already persistent object. This is only useful for members which are configured
   * as lazy (only Object* properties)
   *
   * @param obj the persistent object pointer. Note that this pointer must have been obtained from the KV store
   * @param propertyId the propertyId (1-based index of the declared property). Note that the template type
   * parameter must refer to the (super-)class which declares the member
   */
  template <typename T>
  void loadMember(std::shared_ptr<T> &obj, PropertyAccessBase *pa)
  {
    using Traits = ClassTraits<T>;

    ObjectId objId = ClassTraits<T>::getObjectId(obj);

    ReadBuf rb;
    ClassTraits<T>::load(this, rb, Traits::info->classId, objId, &obj, pa, StoreMode::force_all);
  }

  ClassId getClassId(const std::type_info &ti) {
    return store.typeInfos[ti];
  }

  /**
   * same as abort, but keeps resources allocated for a subsequent renew()
   */
  void reset();
  /**
   * renew a bewviously reset() transaction
   */
  void renew();
  /**
   * abort (close) this transaction, The transaction must not be used afterward
   */
  void abort();
};

#define RAWDATA_API_ASSERT static_assert(TypeTraits<T>::byteSize == sizeof(T), \
"collection data access only supported for fixed-size types with native size equal byteSize");
#define VALUEOBJECT_API_ASSERT static_assert(ClassTraits<V>::keyPropertyId, \
"mapped object type must define ObjectIdAssign and keyPropertyId");

/**
 * Transaction for exclusive read and operations. Opening write transactions while an exclusive read is open
 * will fail with an exception. Likewise creating an exclusive read transcation while a write is ongoing
 */
class FlexisPersistence_EXPORT ExclusiveReadTransaction : public virtual ReadTransaction
{
  virtual bool _getCollectionData(
      CollectionInfo *info, size_t startIndex, size_t length, size_t elementSize, void **data, bool *owned) = 0;

protected:
  ExclusiveReadTransaction(KeyValueStore &store) : ReadTransaction(store) {}

public:
  /**
   * Note that the raw data API is only usable for floating point (float, double) and for integral data types that
   * conform to the LP64 data model. This precludes the long data type on Windows platforms
   *
   * @return a pointer to a memory chunk containing the raw collection data. The memory chunk may be
   * database-owned or copied, depending on whether start and end lie within the same chunk.
   */
  template <typename T> typename
  CollectionData<T>::Ptr getDataCollection(ObjectId collectionId, size_t startIndex, size_t length)
  {
    RAWDATA_API_ASSERT
    void *data;
    bool owned;
    CollectionInfo *ci = getCollectionInfo(collectionId);
    if(!ci) return nullptr;

    if(_getCollectionData(ci, startIndex, length, TypeTraits<T>::byteSize, &data, &owned)) {
      return typename CollectionData<T>::Ptr(new CollectionData<T>(data, owned));
    }
    return nullptr;
  }
};

class CollectionAppenderBase
{
protected:
  CollectionInfo *m_collectionInfo;
  const size_t m_chunkSize;
  WriteTransaction * const m_wtxn;

  WriteBuf &m_writeBuf;
  size_t m_elementCount;

  CollectionAppenderBase(WriteTransaction *wtxn, ObjectId collectionId, size_t chunkSize);
  void startChunk(size_t size);

public:
  void close();
};

/**
 * calculate shallow byte size, i.e. the size of the buffer required for properties that dont't get saved under
 * an individual key
 */
template<typename T>
static size_t calculateBuffer(T *obj, Properties *properties)
{
  size_t size = 0;
  for(unsigned i=0, sz=properties->full_size(); i<sz; i++) {
    auto info = properties->get(i);

    if(!info->enabled) continue;

    //calculate variable size
    ClassTraits<T>::add(obj, info, size);
  }
  return size;
}

/**
 * Transaction for read and write operations. Only one write transaction can be active at a time, and it should
 * be accessed from one thread only
 */
class FlexisPersistence_EXPORT WriteTransaction : public virtual ReadTransaction
{
  template<typename T, typename V> friend class BasePropertyStorage;
  template<typename T, typename V> friend class SimplePropertyStorage;
  template<typename T, typename V> friend class ValueVectorPropertyStorage;
  template<typename T, typename V> friend class ValueSetPropertyStorage;
  template<typename T, typename V> friend class ObjectPropertyStorage;
  template<typename T, typename V> friend class ObjectVectorPropertyStorage;
  template<typename T, typename V> friend class ObjectVectorPropertyStorageEmbedded;
  template<typename T, typename V> friend class ObjectPtrVectorPropertyStorageEmbedded;
  template<typename T, typename V, template<typename> class Ptr> friend class ObjectPtrPropertyStorage;
  template<typename T, typename V> friend class ObjectPtrVectorPropertyStorage;
  template<typename T, typename V, typename KVIter, typename Iter> friend struct ObjectIterPropertyStorage;
  friend class CollectionAppenderBase;

  WriteBuf writeBufStart;
  WriteBuf  *curBuf;

  void writeChunkHeader(size_t startIndex, size_t elementCount);
  void writeObjectHeader(ClassId classId, ObjectId objectId, size_t size);

  /**
   * start a new chunk by allocating memory from the KV store for it. Also write the chunk header for the
   * current chunk, if any
   *
   * @param ci
   * @param chunkSize
   * @param elementCount the number of elements written to the current chunk. Used to write the header. If
   */
  void startChunk(CollectionInfo *collectionInfo, size_t chunkSize, size_t elementCount);

protected:
  const bool m_append;

  WriteTransaction(KeyValueStore &store, bool append=false) : ReadTransaction(store), m_append(append) {
    curBuf = &writeBufStart;
  }

  /**
   * non-polymorphic object removal
   */
  template <typename T>
  bool removeObject(ClassId classId, ObjectId objectId, T &obj)
  {
    using Traits = ClassTraits<T>;

    //first kill all separately stored (vector) properties
    PropertyId propertyId = 0;
    for(unsigned px=0, sz=Traits::properties->full_size(); px < sz; px++) {
      PropertyAccessBase *p = Traits::properties->get(px);
      if(p->type.isVector) remove(classId, objectId, p->id);
    }
    //now remove the object proper
    remove(classId, objectId, 0);
  }

  /**
   * serialize the object to the write buffer
   */
  template <typename T>
  void writeObject(ClassId classId, ObjectId objectId, T &obj, Properties *properties, bool shallow)
  {
    //put data into buffer
    PropertyId propertyId = 0;
    for(unsigned px=0, sz=properties->full_size(); px < sz; px++) {
      //we use the key index (+1) as id
      propertyId++;

      PropertyAccessBase *pa = properties->get(px);
      if(!pa->enabled) continue;

      ClassTraits<T>::save(this, classId, objectId, &obj, pa, shallow ? StoreMode::force_buffer : StoreMode::force_none);
    }
  }

  /**
   * non-polymorphic save. Use in statically typed context
   *
   * @param id the object id
   * @param obj the object to save
   * @param newObject whether the object key needs to be generated
   * @param pa property that has changed. If null, everything will be written
   * @param shallow skip properties that go to separate keys (except pa)
   */
  template <typename T>
  ObjectId saveObject(ObjectId id, T &obj, bool newObject, PropertyAccessBase *pa=nullptr, bool shallow=false)
  {
    using Traits = ClassTraits<T>;

    AbstractClassInfo *classInfo = Traits::info;
    ClassId classId = classInfo->classId;
    ObjectId objectId = newObject ? ++classInfo->maxObjectId : id;

    //create the data buffer
    size_t size = calculateBuffer(&obj, Traits::properties);
    writeBuf().start(size);

    writeObject(classId, objectId, obj, Traits::properties, shallow);
    if(pa && shallow)
      Traits::save(this, classId, id, &obj, pa, StoreMode::force_property);

    if(!putData(classId, objectId, 0, writeBuf()))
      throw persistence_error("data was not saved");

    writeBuf().reset();

    if(newObject) {
      auto ida = Traits::objectIdAccess();
      if(ida) ida->set(obj, objectId);
    }
    return objectId;
  }

  /**
   * polymorphic save
   *
   * @param classId the actual classId of the object to save, possibly a subclass of T
   * @param id the object id
   * @param obj the object to save
   * @param newObject whether the object key needs to be generated
   */
  template <typename T>
  ObjectId saveObject(ClassId classId, ObjectId id, T &obj, bool newObject, PropertyAccessBase *pa=nullptr, bool shallow=false)
  {
    AbstractClassInfo *classInfo = store.objectClassInfos.at(classId);
    if(!classInfo) throw persistence_error("class not registered");

    Properties *properties = store.objectProperties[classId];
    ObjectId objectId = newObject ? ++classInfo->maxObjectId : id;

    if(pa && shallow)
      ClassTraits<T>::save(this, classId, id, &obj, pa, StoreMode::force_property);

    //create the data buffer
    size_t size = calculateBuffer(&obj, properties);
    writeBuf().start(size);
    writeObject(classId, objectId, obj, properties, shallow);

    if(!putData(classId, objectId, 0, writeBuf()))
      throw persistence_error("data was not saved");

    writeBuf().reset();

    if(newObject) {
      auto ida = properties->objectIdAccess<T>();
      if(ida) ida->set(obj, objectId);
    }
    return objectId;
  }

  WriteBuf &writeBuf() {
    return *curBuf;
  }

  void pushWriteBuf() {
    curBuf = curBuf->push();
  }

  void popWriteBuf() {
    curBuf = curBuf->pop();
  }


  struct chunk_helper {
    ClassId classId;
    ObjectId objectId;
    size_t size;
    Properties *properties;

    void set(ClassId cid, ObjectId oid, size_t sz, Properties *props) {
      classId= cid; objectId = oid; size = sz; properties = props;
    }
  };

  template <typename T, template <typename T> class Ptr>
  chunk_helper *prepare_collection(const std::vector<Ptr<T>> &vect, size_t &chunkSize)
  {
    chunk_helper *helpers = new chunk_helper[vect.size()];

    chunkSize = 0;
    for(size_t i=0, vectSize = vect.size(); i<vectSize; i++) {
      ClassId classId = getClassId(typeid(*vect[i]));
      Properties *properties = store.objectProperties[classId];
      AbstractClassInfo *classInfo = store.objectClassInfos[classId];
      ObjectId objectId = ++classInfo->maxObjectId;

      size_t sz = calculateBuffer(&(*vect[i]), properties) + ObjectHeader_sz;
      helpers[i].set(classId, objectId, sz, properties);
      chunkSize += sz;
    }
    return helpers;
  }

  /**
   * save object collection chunk
   *
   * @param vect the collection
   * @param collectionId the id of the collection
   * @param chunkId the chunk index
   * @param poly lookup classes dynamically (slight runtime overhead)
   */
  template <typename T, template <typename T> class Ptr>
  void saveChunk(const std::vector<Ptr<T>> &vect, CollectionInfo *collectionInfo, bool poly)
  {
    if(vect.empty()) return;

    if(poly) {
      size_t chunkSize = 0;
      chunk_helper *helpers = prepare_collection(vect, chunkSize);

      startChunk(collectionInfo, chunkSize, vect.size());

      for(size_t i=0, vectSize = vect.size(); i<vectSize; i++) {
        chunk_helper &helper = helpers[i];

        writeObjectHeader(helper.classId, helper.objectId, helper.size);
        writeObject(helper.classId, helper.objectId, *vect[i], helper.properties, true);
      }
      delete [] helpers;
    }
    else {
      size_t chunkSize = 0;
      size_t *sizes = new size_t[vect.size()];

      for(size_t i=0, vectSize = vect.size(); i<vectSize; i++) {
        sizes[i] = calculateBuffer(&(*vect[i]), ClassTraits<T>::properties) + ObjectHeader_sz;
        chunkSize += sizes[i];
      }
      startChunk(collectionInfo, chunkSize, vect.size());

      for(size_t i=0, vectSize = vect.size(); i<vectSize; i++) {
        using Traits = ClassTraits<T>;

        AbstractClassInfo *classInfo = Traits::info;
        ClassId classId = classInfo->classId;
        ObjectId objectId = ++classInfo->maxObjectId;

        size_t size = sizes[i];
        writeObjectHeader(classId, objectId, size);
        writeObject(classId, objectId, *vect[i], Traits::properties, true);
      }
      delete [] sizes;
    }
  }

  /**
   * save value collection chunk
   *
   * @param vect the collection
   * @param collectionId the id of the collection
   */
  template <typename T>
  void saveChunk(const std::vector<T> &vect, CollectionInfo *ci)
  {
    if(vect.empty()) return;

    size_t chunkSize = 0;
    for(size_t i=0, vectSize = vect.size(); i<vectSize; i++)
      chunkSize += ValueTraits<T>::size(vect[i]);

    startChunk(ci, chunkSize, vect.size());

    for(size_t i=0, vectSize = vect.size(); i<vectSize; i++)
      ValueTraits<T>::putBytes(writeBuf(), vect[i]);
  }

  /**
   * save raw data collection chunk
   *
   * @param array the raw data array
   * @param arraySize the number of items in array
   * @param ci the collection metadata
   */
  template <typename T>
  void saveChunk(const T *array, size_t arraySize, CollectionInfo *ci)
  {
    if(!arraySize) return;

    size_t chunkSize = arraySize * sizeof(T);

    startChunk(ci, chunkSize, arraySize);
    writeBuf().append((byte_t *)array, chunkSize);
  }

  /**
   * save a sub-object data buffer
   */
  virtual bool putData(ClassId classId, ObjectId objectId, PropertyId propertyId, WriteBuf &buf) = 0;

  /**
   * save a sub-object data buffer
   */
  virtual bool allocData(ClassId classId, ObjectId objectId, PropertyId propertyId, size_t size, byte_t **data) = 0;

  /**
   * remove an object from the KV store
   */
  virtual bool remove(ClassId classId, ObjectId objectId, PropertyId propertyId) = 0;

  virtual void doCommit() = 0;

public:
  virtual ~WriteTransaction()
  {
    //assume all was popped
    curBuf->deleteChain();
  }

  void commit();

  /**
   * put a new object into the KV store. A key will be generated that consists of [class identifier/object identifier]. The
   * key, which will be unique within the class identifier, will be stored inside the shared_ptr
   *
   * @return the object identifier
   */
  template <typename T>
  std::shared_ptr<T> putObject(T *obj)
  {
    ObjectId oid = saveObject<T>(0, *obj, true);
    return make_ptr(obj, oid);
  }

  /**
   * put a new object into the KV store. A key will be generated that consists of [class identifier/object identifier]. The
   * key, which will be unique within the class identifier, will be stored inside the shared_ptr
   */
  template <typename T>
  void putObject(std::shared_ptr<T> obj)
  {
    ObjectId oid = saveObject<T>(0, *obj, true);
    set_objectid(obj, oid);
  }

  /**
   * put a new object into the KV store. A key will be generated that consists of [class identifier/object identifier]. The
   * object identifier will be unique within the class identifier.
   *
   * @return the object identifier
   */
  template <typename T>
  ObjectId putObject(T &obj)
  {
    return saveObject<T>(0, obj, true);
  }

  /**
   * save an object into the KV store. If the shared_ptr carries an ObjectId, the object will be written under the existing key.
   * Otherwise, the object will be stored under a new key (whiuch is again stored inside the shared_ptr)
   *
   * @param obj a persistent object pointer, which must have been obtained from KV (see make_obj, make_ptr)
   * @return the objectId
   */
  template <typename T>
  ObjectId saveObject(const std::shared_ptr<T> &obj)
  {
    ObjectId oid = ClassTraits<T>::getObjectId(obj);

    if(oid) {
      saveObject<T>(oid, *obj, false);
    }
    else {
      oid = saveObject<T>(0, *obj, true);
      set_objectid(obj, oid);
    }
    return oid;
  }

  /**
   * update an existing object in the KV store.
   *
   * @param objectId the object identifier as returned from a previous call to putObject
   * @param obj the object to update
   */
  template <typename T>
  void updateObject(ObjectId objectId, T &obj)
  {
    saveObject<T>(objectId, obj, false);
  }

  /**
   * update a member variable of the given, already persistent object. The current variable state is written to the
   * store
   *
   * @param objectId an Id obtained from a previous put
   * @param obj the persistent object
   * @param propertyId the propertyId (1-based index of the declared property). Note that the template type
   * parameter must refer to the (super-)class which declares the member
   */
  template <typename T>
  void updateMember(const ObjectId objId, T &obj, PropertyAccessBase *pa, bool shallow=false)
  {
    ClassId classId = getClassId(typeid(obj));

    switch(pa->storage->layout) {
      case StoreLayout::property:
        //property goes to a separate key, no need to touch the object buffer
        ClassTraits<T>::save(this, classId, objId, &obj, pa, shallow ? StoreMode::force_buffer : StoreMode::force_all);
        break;
      case StoreLayout::embedded_key:
        //save property value and shallow buffer
        saveObject<T>(classId, objId, obj, false, pa, true);
        break;
      case StoreLayout::all_embedded:
        //shallow buffer only
        saveObject<T>(classId, objId, obj, false, nullptr, true);
    }
  }

  /**
   * update a member variable of the given, already persistent object. The current variable state is written to the
   * store
   *
   * @param obj the persistent object pointer. Note that this pointer must have been obtained from the KV store
   * @param propertyId the propertyId (1-based index of the declared property). Note that the template type
   * parameter must refer to the (super-)class which declares the member
   */
  template <typename T>
  void updateMember(const std::shared_ptr<T> &obj, PropertyAccessBase *pa, bool shallow=false)
  {
    ObjectId objId = ClassTraits<T>::getObjectId(obj);
    updateMember(objId, *obj, pa, shallow);
  }

  /**
   * insert an attached member collection (see explanation in accompanying get function)
   *
   * @param obj the object this collection is attached to
   * @param propertyId a property Id outside the Id space of obj's class
   * @param vect the contents of the collection
   */
  template <typename T, typename V>
  void putCollection(std::shared_ptr<T> &obj, PropertyId propertyId, const std::vector<std::shared_ptr<V>> &vect,
                     bool saveMembers=true)
  {
    ObjectId objId = ClassTraits<T>::getObjectId(obj);

    ClassId classId = getClassId(typeid(*obj));
    byte_t *data;
    size_t bufSz = vect.size()*StorageKey::byteSize+4;

    if(!allocData(classId, objId, propertyId, bufSz, &data))
      throw persistence_error("allocData failed");

    writeBuf().start(data, bufSz);
    writeBuf().appendInteger<size_t>(vect.size(), 4);

    for(auto &v : vect) {
      ClassId classId = getClassId(typeid(*v));
      ObjectId objectId;

      if(saveMembers) {
        pushWriteBuf();
        objectId = saveObject(v);
        popWriteBuf();
      }
      else {
        objectId = ClassTraits<V>::getObjectId(v);
        if(!objectId) {
          pushWriteBuf();
          objectId = saveObject(0, v, true);
          popWriteBuf();
        }
      }
      writeBuf().append(classId, objectId, 0);
    }
  }

  /**
   * add or remove an element to an attached member collection
   *
   * @param obj the object the collection is attached to
   * @param propertyId the attached propertyId (not from the mappings)
   * @val the value to add. The value will be saved prior to being inserted into the collection
   */
  template <typename T, typename V>
  bool updateCollection(std::shared_ptr<T> &obj, PropertyId propertyId, const std::shared_ptr<V> &val, bool remove=false)
  {
    ObjectId objectId = ClassTraits<T>::getObjectId(obj);

    ReadBuf buf;
    ClassId classId = getClassId(typeid(*obj));
    getData(buf, classId, objectId, propertyId);
    if(buf.null()) throw persistence_error("collection does not exist");

    size_t elementCount = buf.readInteger<size_t>(4);

    ClassId valueClassId = getClassId(typeid(*val));
    ObjectId valueId = ClassTraits<V>::getObjectId(val);
    byte_t *slotStart = nullptr;

    for(size_t i=0; i < elementCount; i++) {
      StorageKey sk;
      buf.read(sk);

      if(sk.classId == valueClassId && sk.objectId == valueId) {
        if(!remove) {
          saveObject(val);
          return false;
        };
        slotStart = buf.cur()-StorageKey::byteSize;
        break;
      }
    }

    if(remove) {
      if(!slotStart) return false;
      writeBuf().start(buf.size()-StorageKey::byteSize);
      writeBuf().append(buf.data(), slotStart-buf.data());
      writeBuf().append(slotStart+StorageKey::byteSize, buf.size()-StorageKey::byteSize);
      putData(classId, objectId, propertyId, writeBuf());
      return true;
    }
    else {
      if(!valueId) {
        valueId = saveObject(0, val, true);
      }
      writeBuf().start(buf.size()+StorageKey::byteSize);
      writeBuf().append(buf.data(), buf.size());
      writeBuf().append(valueClassId, valueId, 0);
    }
  }

  /**
   * save a top-level (chunked) object collection.
   *
   * @param vect the collection contents
   */
  template <typename T, template <typename> class Ptr> ObjectId putCollection(const std::vector<Ptr<T>> &vect)
  {
    CollectionInfo *ci = new CollectionInfo(++store.m_maxCollectionId);
    m_collectionInfos[ci->collectionId] = ci;

    saveChunk(vect, ci, ClassTraits<T>::info->isPoly());

    return ci->collectionId;
  }

  /**
   * save a top-level (chunked) member object collection. After saving the collection, the member object
   * will be initialized with the collectionId and a pointer to the KV store
   *
   * @param o the object that holds the member
   * @param pa the property accessor, usually obtained via PROPERTY macro
   * @param vect the collection contents
   */
  template <typename O, typename T>
  ObjectId putCollection(O &o, PropertyAccessBase *pa, const std::vector<std::shared_ptr<T>> &vect)
  {
    CollectionInfo *ci = new CollectionInfo(++store.m_maxCollectionId);
    m_collectionInfos[ci->collectionId] = ci;

    saveChunk(vect, ci, ClassTraits<T>::info->isPoly());

    void * ib = ClassTraits<O>::initMember(&o, pa);
    if(!ib)
      throw persistence_error(std::string("property ")+pa->name+" is not a collection member");

    //bad luck if pa->storage was not an ObjectIterPropertyStorage
    ((IterPropertyBackend<T> *)ib)->setCollectionId(ci->collectionId);
    ((IterPropertyBackend<T> *)ib)->setKVStore(&store);

    return ci->collectionId;
  }

  /**
   * save a top-level (chunked) value collection.
   *
   * @param vect the collection contents
   */
  template <typename T>
  ObjectId putValueCollection(const std::vector<T> &vect)
  {
    CollectionInfo *ci = new CollectionInfo(++store.m_maxCollectionId);
    m_collectionInfos[ci->collectionId] = ci;

    saveChunk(vect, ci);

    return ci->collectionId;
  }

  /**
   * save a top-level (chunked) raw data collection. Note that the rwaw data API is only usable for
   * floating point (float, double) and for integral data types that conform to the LP64 data model.
   * This precludes the long data type on Windows platforms
   *
   * @param array the collection contents
   * @param arraySize length of the contents
   */
  template <typename T>
  ObjectId putDataCollection(const T* array, size_t arraySize)
  {
    RAWDATA_API_ASSERT
    CollectionInfo *ci = new CollectionInfo(++store.m_maxCollectionId);
    m_collectionInfos[ci->collectionId] = ci;

    saveChunk(array, arraySize, ci);

    return ci->collectionId;
  }

  /**
   * append to a top-level (chunked) object collection.
   *
   * @param vect the collection contents
   * @param chunkSize size of chunk
   */
  template <typename T, template <typename T> class Ptr>
  void appendCollection(ObjectId collectionId, const std::vector<Ptr<T>> &vect)
  {
    CollectionInfo *ci = getCollectionInfo(collectionId);
    if(!ci) throw persistence_error("collection not found");

    saveChunk(vect, ci, ClassTraits<T>::info->isPoly());
  }

  /**
   * append to a top-level (chunked) value collection.
   *
   * @param vect the collection contents
   * @param chunkSize size of keys chunk
   */
  template <typename T>
  void appendValueCollection(ObjectId collectionId, const std::vector<T> &vect, size_t chunkSize = CHUNKSIZE)
  {
    CollectionInfo *ci= getCollectionInfo(collectionId);
    if(!ci) throw persistence_error("collection not found");

    saveChunk(vect, ci);
  }

  /**
   * append to a top-level (chunked) raw data collection. Note that the raw data API is only usable for floating-point
   * (float, double) and for integral data types that conform to the LP64 data model. This precludes the long data
   * type on Windows platforms
   *
   * @param vect the collection contents
   * @param chunkSize size of keys chunk
   */
  template <typename T>
  void appendDataCollection(ObjectId collectionId, const T *data, size_t dataSize, size_t chunkSize = CHUNKSIZE)
  {
    RAWDATA_API_ASSERT
    CollectionInfo *ci = getCollectionInfo(collectionId);
    if(!ci) throw persistence_error("collection not found");

    saveChunk(data, dataSize, ci);
  }

  template <typename T>
  void deleteObject(ObjectId objectId, T &obj) {
    removeObject(objectId, obj);
  }

  template <typename T>
  void deleteObject(std::shared_ptr<T> obj) {
    ClassId cid = getClassId(typeid(*obj));
    ObjectId oid = ClassTraits<T>::getObjectId(obj);
    removeObject(cid, oid, obj);
  }

  /**
  * appender for sequentially extending a top-level, chunked object collection
  */
  template <typename T>
  class ObjectCollectionAppender : public CollectionAppenderBase
  {
    const bool m_poly;
    ObjectClassInfos * const m_objectClassInfos;
    ObjectProperties * const m_objectProperties;

  protected:
    void _put(T &obj)
    {
      ClassId cid;
      ObjectId oid;

      Properties *properties;
      if(m_poly) {
        cid = m_wtxn->getClassId(typeid(obj));

        AbstractClassInfo *classInfo = m_objectClassInfos->at(cid);
        oid = ++classInfo->maxObjectId;
        properties = m_objectProperties->at(cid);
      }
      else {
        cid = ClassTraits<T>::info->classId;
        oid = ++ClassTraits<T>::info->maxObjectId;
        properties = ClassTraits<T>::properties;
      }
      size_t size = calculateBuffer(&obj, properties) + ObjectHeader_sz;

      if(m_writeBuf.avail() < size) startChunk(size);

      m_wtxn->writeObjectHeader(cid, oid, size);
      m_wtxn->writeObject(cid, oid, obj, properties, false);

      m_elementCount++;
    }

  public:
    using Ptr = std::shared_ptr<ObjectCollectionAppender>;

    ObjectCollectionAppender(WriteTransaction *wtxn, ObjectId collectionId,
                             size_t chunkSize, ObjectClassInfos *objectClassInfos, ObjectProperties *objectProperties, bool poly)
        : CollectionAppenderBase(wtxn, collectionId, chunkSize),
          m_objectClassInfos(objectClassInfos), m_objectProperties(objectProperties), m_poly(poly)
    {
    }

    void put(std::shared_ptr<T> obj) {
      _put(*obj);
    }

    void put(T *obj) {
      _put(*obj);
    }
  };

  /**
  * appender for sequentially extending a top-level, chunked value collection
  */
  template <typename T>
  class ValueCollectionAppender : public CollectionAppenderBase
  {
  public:
    using Ptr = std::shared_ptr<ValueCollectionAppender>;

    ValueCollectionAppender(WriteTransaction *wtxn, ObjectId collectionId, size_t chunkSize)
        : CollectionAppenderBase(wtxn, collectionId, chunkSize)
    {}

    void put(T val)
    {
      size_t sz = TypeTraits<T>::byteSize;
      if(sz == 0) sz = ValueTraits<T>::size(val);

      size_t avail = m_writeBuf.avail();

      if(avail < sz) startChunk(sz);

      ValueTraits<T>::putBytes(m_writeBuf, val);
      m_elementCount++;
    }
  };

  /**
  * appender for sequentially extending a top-level, chunked value collection
  */
  template <typename T>
  class DataCollectionAppender : public CollectionAppenderBase
  {
  public:
    using Ptr = std::shared_ptr<DataCollectionAppender>;

    DataCollectionAppender(WriteTransaction *wtxn, ObjectId collectionId, size_t chunkSize)
        : CollectionAppenderBase(wtxn, collectionId, chunkSize)
    {}

    void put(T *val, size_t dataSize)
    {
      size_t avail = m_writeBuf.avail();
      byte_t *data = (byte_t *)val;

      if(avail >= sizeof(T)) {
        m_writeBuf.append(data, avail);
        m_elementCount += avail / sizeof(T);
        dataSize -= avail;
        data += avail;
      }
      if(dataSize) {
        startChunk(dataSize < m_chunkSize ? m_chunkSize : dataSize);
        m_writeBuf.append(data, dataSize);
        m_elementCount += dataSize / sizeof(T);
      }
    }
  };

  /**
   * create an appender for the given top-level object collection
   *
   * @param collectionId the id of a top-level collection
   * @param chunkSize the chunk size
   * @return an appender over the contents of the collection.
   */
  template <typename V> typename ObjectCollectionAppender<V>::Ptr appendCollection(
      ObjectId collectionId, size_t chunkSize = CHUNKSIZE)
  {
    return typename ObjectCollectionAppender<V>::Ptr(new ObjectCollectionAppender<V>(
        this, collectionId, chunkSize, &store.objectClassInfos, &store.objectProperties, ClassTraits<V>::info->isPoly()));
  }

  /**
   * create an appender for the given top-level raw-data collection
   *
   * @param collectionId the id of a top-level collection
   * @param chunkSize the chunk size
   * @return an appender over the contents of the collection.
   */
  template <typename V> typename ValueCollectionAppender<V>::Ptr appendValueCollection(
      ObjectId collectionId, size_t chunkSize = CHUNKSIZE)
  {
    return typename ValueCollectionAppender<V>::Ptr(new ValueCollectionAppender<V>(this, collectionId, chunkSize));
  }

  /**
   * create an appender for the given top-level raw-data collection
   *
   * @param collectionId the id of a top-level collection
   * @param chunkSize the keychunk size
   * @return an appender over the contents of the collection.
   */
  template <typename T> typename ValueCollectionAppender<T>::Ptr appendDataCollection(
      ObjectId collectionId, size_t chunkSize = CHUNKSIZE)
  {
    RAWDATA_API_ASSERT
    return typename DataCollectionAppender<T>::Ptr(new DataCollectionAppender<T>(this, collectionId, chunkSize));
  }
};

/**
 * storage class template for base types that go directly into the shallow buffer
 */
template<typename T, typename V>
struct BasePropertyStorage : public StoreAccessBase
{
  size_t size(const byte_t *buf) const override {
    return TypeTraits<V>::byteSize;
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    return TypeTraits<V>::byteSize;
  }
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    V val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    ValueTraits<V>::putBytes(tr->writeBuf(), val);
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    V val;
    T *tp = reinterpret_cast<T *>(obj);
    ValueTraits<V>::getBytes(buf, val);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage class template for cstring, with dynamic size calculation (type.byteSize is 0). Note that after loading
 * the data store, the pointed-to belongs to the datastore and will in all likelihood become invalid by the end of
 * the trasaction. It is up to the application to copy the value away (or use std::string)
 */
template<typename T>
struct BasePropertyStorage<T, const char *> : public StoreAccessBase
{
  size_t size(const byte_t *buf) const override {
    return strlen(buf)+1;
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    const char * val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    return strlen(val) + 1;
  }
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    const char * val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    ValueTraits<const char *>::putBytes(tr->writeBuf(), val);
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    const char * val;
    T *tp = reinterpret_cast<T *>(obj);
    ValueTraits<const char *>::getBytes(buf, val);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template class for std::string, with dynamic size calculation (type.byteSize is 0)
 */
template<typename T>
struct BasePropertyStorage<T, std::string> : public StoreAccessBase
{
  size_t size(const byte_t *buf) const override {
    return strlen((const char *)buf)+1;
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    std::string val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    return val.length() + 1;
  }
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::string val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    ValueTraits<std::string>::putBytes(tr->writeBuf(), val);
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::string val;
    T *tp = reinterpret_cast<T *>(obj);
    ValueTraits<std::string>::getBytes(buf, val);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for ClassId-typed properties. The ClassId (which is already part of the key) is mapped to an
 * object property
 */
template<typename T>
struct ObjectIdStorage : public StoreAccessBase
{
  size_t size(const byte_t *buf) const override {
    return 0;
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    return 0;
  }
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    //not saved, only loaded
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, objectId);
  }
};

/**
 * storage template for std::vector of simple values. All values are serialized into one consecutive buffer which is
 * stored under a property key for the given object.
 */
template<typename T, typename V>
struct ValueVectorPropertyStorage : public StoreAccessPropertyKey
{
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t psz = 0;
    for(auto &v : val) psz += ValueTraits<V>::size(v);
    if(psz) {
      WriteBuf propBuf(psz);

      for(auto v : val) ValueTraits<V>::putBytes(propBuf, v);

      if(!tr->putData(classId, objectId, pa->id, propBuf))
        throw persistence_error("data was not saved");
    }
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<V> val;

    ReadBuf readBuf;
    tr->getData(readBuf, classId, objectId, pa->id);

    while(!readBuf.atEnd()) {
      V v;
      ValueTraits<V>::getBytes(readBuf, v);
      val.push_back(v);
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for sets of simnple vvalues. Similar to value vector, but based on a std::set
 */
template<typename T, typename V>
struct ValueSetPropertyStorage : public StoreAccessPropertyKey
{
  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::set<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t psz = 0;
    for(auto &v : val) psz += ValueTraits<V>::size(v);
    if(psz) {
      WriteBuf propBuf(psz);

      for(auto v : val) ValueTraits<V>::putBytes(propBuf, v);

      if(!tr->putData(classId, objectId, pa->id, propBuf))
        throw persistence_error("data was not saved");
    }
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::set<V> val;

    ReadBuf readBuf;
    tr->getData(readBuf, classId, objectId, pa->id);

    while(!readBuf.atEnd()) {
      V v;
      ValueTraits<V>::getBytes(readBuf, v);
      val.insert(v);
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for mapped non-pointer object references. Since the object is referenced by value value in the enclosing
 * class, storage can only be non-polymorphic. The object is serialized into a separate buffer, but the key is written to the
 * enclosing object's buffer. the referenced object is required to hold an ObjectId-typed member variable which is referenced
 * through the keyPropertyId
 */
template<typename T, typename V> struct ObjectPropertyStorage : public StoreAccessEmbeddedKey
{
  VALUEOBJECT_API_ASSERT

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    V val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    //save the value object
    tr->pushWriteBuf();

    ClassId childClassId = ClassTraits<V>::info->classId;
    auto ida = ClassTraits<V>::objectIdAccess();

    ObjectId childId = ida->get(val);
    if(childId) tr->updateObject(childId, val);
    else childId = tr->putObject<V>(val);

    tr->popWriteBuf();

    //save the key in this objects write buffer
    tr->writeBuf().append(childClassId, childId, 0);
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    StorageKey sk;
    buf.read(sk);

    V *v = tr->loadObject<V>(sk.objectId);
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, *v);

    //inefficient - delete the transport object, which was copied into the vector
    delete tp;
  }
};

/**
 * base template for access to ObjectId from arbitrary pointer types
 */
template <typename V, template <typename> class Ptr> struct OidAccess
{
  static Ptr<V> make(V *v, ObjectId oid) {
    Ptr<V> ptr(v, oid);
    return ptr;
  }

  static ObjectId getObjectId(Ptr<V> ptr) {
    return ptr.objectid;
  }

  static void setObjectId(ObjectId oid, Ptr<V> &ptr) {
    ptr.objectid = oid;
  }
};

/**
 * template specialization for access to ObjectId from std::shared_ptr
 */
template <typename V> struct OidAccess<V, std::shared_ptr>
{
  static std::shared_ptr<V> make(V *v, ObjectId oid) {
    std::shared_ptr<V> ptr = make_ptr(v, oid);
    return ptr;
  }

  static ObjectId getObjectId(ClassId cid, std::shared_ptr<V> ptr) {
    ObjectId oid = ClassTraits<V>::getObjectId(ptr);
    return oid;
  }

  static void setObjectId(ObjectId oid, std::shared_ptr<V> ptr) {
    set_objectid(ptr, oid);
  }
};

/**
 * storage template for mapped object references. Pointer-based, fully polymorphic
 */
template<typename T, typename V, template<typename> class Ptr>
class ObjectPtrPropertyStorage : public StoreAccessEmbeddedKey
{
protected:
  bool m_lazy;

public:
  ObjectPtrPropertyStorage(bool lazy=false) : m_lazy(lazy) {}

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    Ptr<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    ClassId childClassId = 0;
    ObjectId childId = 0;
    if(val) {
      //save the pointed-to object
      childClassId = tr->getClassId(typeid(*val));
      childId = OidAccess<V, Ptr>::getObjectId(childClassId, val);

      if(mode != StoreMode::force_buffer) {
        tr->pushWriteBuf();
        if(childId) {
          tr->saveObject<V>(childClassId, childId, *val, false);
        }
        else {
          childId = tr->saveObject<V>(childClassId, 0, *val, true);
        }
        tr->popWriteBuf();

        //update the property so that it holds the object id
        OidAccess<V, Ptr>::setObjectId(childId, val);
      }
    }

    if(mode != StoreMode::force_property) {
      //save the key in this objects write buffer
      tr->writeBuf().append(childClassId, childId, 0);
    }
  }
  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(m_lazy && mode == StoreMode::force_none) {
      buf.read(StorageKey::byteSize);
      return;
    }

    StorageKey sk;
    buf.read(sk);

    if(sk.classId > 0) {
      V *v = tr->loadObject<V>(sk.objectId);
      Ptr<V> vp = OidAccess<V, Ptr>::make(v, sk.objectId);
      T *tp = reinterpret_cast<T *>(obj);
      ClassTraits<T>::get(*tp, pa, vp);
    }
  }
};

template<typename T, typename V, template<typename> class Ptr>
class ObjectPtrPropertyDeferredStorage : public ObjectPtrPropertyStorage<T, V, Ptr>
{
public:
  ObjectPtrPropertyDeferredStorage() : ObjectPtrPropertyStorage<T, V, Ptr>(true) {}

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(mode == StoreMode::force_none) {
      const byte_t *data = buf.read(StorageKey::byteSize);
      ObjectId oid = *(ObjectId *)(data+ClassId_sz);

      T *tp = reinterpret_cast<T *>(obj);
      Ptr<V> val = OidAccess<V, Ptr>::make(nullptr, oid);
      ClassTraits<T>::put(*tp, pa, val);
      return;
    }
    else
      ObjectPtrPropertyStorage<T, V, Ptr>::load(tr, buf, classId, objectId, obj, pa, mode);
  }

  ObjectId oid(void *obj, const PropertyAccessBase *pa) {
    Ptr<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);
    return OidAccess<V, Ptr>::getObjectId(val);
  }
};

/**
 * storage template for mapped object vectors. Value-based, therefore not polymorphic. Vector element objects are required
 * to hold an ObjectId-typed member variable which is referenced through the keyPropertyId
 */
template<typename T, typename V> class ObjectVectorPropertyStorage : public StoreAccessPropertyKey
{
  VALUEOBJECT_API_ASSERT

  bool m_lazy;
public:
  ObjectVectorPropertyStorage(bool lazy) : m_lazy(lazy) {}

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(m_lazy && mode == StoreMode::force_none) return;

    std::vector<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t psz = StorageKey::byteSize * val.size();
    WriteBuf propBuf(psz);

    tr->pushWriteBuf();
    ClassId childClassId = ClassTraits<V>::info->classId;
    auto ida = ClassTraits<V>::objectIdAccess();

    for(V &v : val) {
      ObjectId childId = ida->get(v);

      if(mode != StoreMode::force_buffer) {
        if (childId) tr->updateObject<V>(childId, v);
        else childId = tr->putObject<V>(v);
      }
      propBuf.append(childClassId, childId, 0);
    }
    tr->popWriteBuf();

    //vector goes into a separate property key
    if(!tr->putData(classId, objectId, pa->id, propBuf))
      throw persistence_error("data was not saved");
  }

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(m_lazy && mode == StoreMode::force_none) return;

    std::vector<V> val;

    //load vector base (array of object keys) data from property key
    ReadBuf readBuf;
    tr->getData(readBuf, classId, objectId, pa->id);

    if(!readBuf.null()) {
      StorageKey sk;
      while(readBuf.read(sk)) {
        V obj;
        tr->loadObject(sk.objectId, obj);
        val.push_back(obj);
      }
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for vectors of mapped objects where the objects are directly serialized into the enclosing object's buffer.
 * The vector elements receive an object Id that is equal vector index + 1. This ID is thus not valid outside the vector.
 * Value-based, therefore not polymorphic.
 *
 * Only the shallow object buffer is saved, non-embeddable members are ignored during serialization
 */
template<typename T, typename V> class ObjectVectorPropertyStorageEmbedded : public StoreAccessBase
{
public:
  size_t size(const byte_t *buf) const override {
    unsigned vectSize = read_integer<unsigned>(buf, 4);
    size_t sz = ClassTraits<V>::properties->fixedSize;
    if(sz) {
      unsigned objSize = read_integer<unsigned>(buf, 4);
      if(objSize != sz) throw persistence_error("invalid object buffer");
      return vectSize * (objSize + 4) + 4;
    }
    else {
      for(unsigned i=0; i<vectSize; i++) {
        unsigned objSize = read_integer<unsigned>(buf, 4);
        buf += 4 + objSize;
        sz += 4 + objSize;
      }
      return sz + 4;
    }
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    std::vector<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t sz = ClassTraits<V>::properties->fixedSize;
    if(sz) return val.size() * (sz + 4) + 4;

    for(V &v : val)
      sz += calculateBuffer(&v, ClassTraits<V>::properties) + 4;
    return sz + 4;
  }

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<V> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    tr->writeBuf().appendInteger((unsigned)val.size(), 4);
    ClassId childClassId = ClassTraits<V>::info->classId;
    PropertyId childObjectId = 0;
    size_t fsz = ClassTraits<V>::properties->fixedSize;
    for(V &v : val) {
      size_t sz = fsz ? fsz : calculateBuffer(&v, ClassTraits<V>::properties);

      tr->writeBuf().appendInteger(sz, 4);
      tr->writeObject(childClassId, ++childObjectId, v, ClassTraits<V>::properties, true);
    }
  }

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<V> val;

    ClassId childClassId = ClassTraits<V>::info->classId;
    PropertyId childObjectId = 0;

    unsigned sz = buf.readInteger<unsigned>(4);
    for(size_t i=0; i< sz; i++) {
      V v;
      buf.readInteger<unsigned>(4);
      readObject(tr, buf, v, childClassId, ++childObjectId);
      val.push_back(v);
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for vectors of pointers to mapped objects where the objects are directly serialized into the enclosing object's
 * buffer. The vector elements receive an object Id that is equal vector index + 1. This ID is thus not valid outside the vector.
 * Fully polymorphic
 *
 * Only the shallow object buffer is saved, non-embeddable members are ignored during serialization
 */
template<typename T, typename V> class ObjectPtrVectorPropertyStorageEmbedded : public StoreAccessBase
{
public:
  size_t size(const byte_t *buf) const override {
    unsigned vectSize = read_integer<unsigned>(buf, 4);
    size_t sz = 0;
    for(unsigned i=0; i<vectSize; i++) {
      unsigned objSize = read_integer<unsigned>(buf+ClassId_sz, 4);
      buf += objSize + 4;
      sz += ClassId_sz + 4 + objSize;
    }
    return sz + 4;
  }
  size_t size(void *obj, const PropertyAccessBase *pa) override {
    std::vector<std::shared_ptr<V>> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t sz = 0;
    for(std::shared_ptr<V> &v : val) {
      sz += ClassTraits<V>::bufferSize(&(*v)) + ClassId_sz + 4;
    }
    return sz + 4;
  }

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<std::shared_ptr<V>> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    tr->writeBuf().appendInteger((unsigned)val.size(), 4);
    PropertyId childObjectId = 0;
    for(std::shared_ptr<V> &v : val) {
      ClassId childClassId;
      size_t sz = ClassTraits<V>::bufferSize(&(*v), &childClassId);

      tr->writeBuf().appendInteger(childClassId, ClassId_sz);
      tr->writeBuf().appendInteger(sz, 4);
      tr->writeObject(childClassId, ++childObjectId, *v, ClassTraits<V>::getProperties(childClassId), true);
    }
  }

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::vector<std::shared_ptr<V>> val;

    PropertyId childObjectId = 0;

    unsigned len = buf.readInteger<unsigned>(4);
    for(size_t i=0; i<len; i++) {
      ClassId childClassId = buf.readInteger<ClassId>(ClassId_sz);
      unsigned sz = buf.readInteger<unsigned>(4);

      ClassInfo<V> *vi = FIND_CLS(V, childClassId);
      if(!vi) {
        buf.mark();
        V *vp = ClassTraits<V>::getSubstitute();
        if(vp) {
          readObject<V>(tr, buf, *vp, childClassId, ++childObjectId, StoreMode::force_buffer);
          val.push_back(std::shared_ptr<V>(vp));
        }
        buf.unmark(sz);
      }
      else {
        V *vp = readObject<V>(tr, buf, childClassId, ++childObjectId, vi);
        val.push_back(std::shared_ptr<V>(vp));
      }
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for vectors of pointers to mapped objects. Collection objects are saved under individual top-level keys.
 * The collection itself holds copies of all member keys in a shallow buffer and is stored under a property key for the enclosing
 * object.
 *
 * Fully polymorphic
 */
template<typename T, typename V> class ObjectPtrVectorPropertyStorage : public StoreAccessPropertyKey
{
  bool m_lazy;

public:
  ObjectPtrVectorPropertyStorage(bool lazy) : m_lazy(lazy) {}

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(m_lazy && mode == StoreMode::force_none) return;

    std::vector<std::shared_ptr<V>> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    size_t psz = StorageKey::byteSize * val.size();
    WriteBuf propBuf(psz);

    tr->pushWriteBuf();
    for(std::shared_ptr<V> &v : val) {
      ClassId childClassId = tr->getClassId(typeid(*v));
      ObjectId childId = ClassTraits<V>::getObjectId(v, false);

      if(mode != StoreMode::force_buffer) {
        if(childId) {
          tr->saveObject<V>(childClassId, childId, *v, false);
        }
        else {
          childId = tr->saveObject<V>(childClassId, 0, *v, true);
          set_objectid(v, childId, false);
        }
      }

      propBuf.append(childClassId, childId, 0);
    }
    tr->popWriteBuf();

    //vector goes into a separate property key
    if(!tr->putData(classId, objectId, pa->id, propBuf))
      throw persistence_error("data was not saved");
  }

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    if(m_lazy && mode == StoreMode::force_none) return;

    std::vector<std::shared_ptr<V>> val;

    //load vector base (array of object keys) data from property key
    ReadBuf readBuf;
    tr->getData(readBuf, classId, objectId, pa->id);

    if(!readBuf.null()) {
      StorageKey sk;
      while(readBuf.read(sk)) {
        ClassInfo<V> *vi = FIND_CLS(V, sk.classId);
        if(!vi) {
          V *vp = ClassTraits<V>::getSubstitute();
          if(vp) {
            tr->loadSubstitute<V>(*vp, sk.classId, sk.objectId);
            val.push_back(make_ptr(vp, sk.objectId));
          }
        }
        else {
          V *obj = tr->loadObject<V>(sk.classId, sk.objectId);
          val.push_back(make_ptr(obj, sk.objectId));
        }
      }
    }
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::get(*tp, pa, val);
  }
};

/**
 * storage template for collection iterator members. The collection id is saved within the enclosing object's buffer.
 * Storing the collection proper is done externally, or through the iterator object
 */
template<typename T, typename V, typename KVIter, typename Iter>
struct ObjectIterPropertyStorage : public StoreAccessPropertyKey
{
  static_assert(std::is_base_of<IterPropertyBackend<V>, KVIter>::value, "KVIter must subclass IterPropertyBackend<V>");
  static_assert(std::is_base_of<Iter, KVIter>::value, "KVIter must subclass Iter");

  void *initMember(void *obj, const PropertyAccessBase *pa) override
  {
    T *tp = reinterpret_cast<T *>(obj);
    KVIter *it = new KVIter();
    auto ib = std::shared_ptr<KVIter>(it);
    ClassTraits<T>::get(*tp, pa, ib);

    return static_cast<IterPropertyBackend<V> *>(it);
  }

  void save(WriteTransaction *tr,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    std::shared_ptr<Iter> val;
    T *tp = reinterpret_cast<T *>(obj);
    ClassTraits<T>::put(*tp, pa, val);

    IterPropertyBackendPtr<V> ib = std::dynamic_pointer_cast<IterPropertyBackend<V>>(val);
    tr->writeBuf().appendRaw(ib ? ib->getCollectionId() : 0);
  }

  void load(ReadTransaction *tr, ReadBuf &buf,
            ClassId classId, ObjectId objectId, void *obj, const PropertyAccessBase *pa, StoreMode mode) override
  {
    T *tp = reinterpret_cast<T *>(obj);
    ObjectId collectionId = buf.readRaw<ObjectId>();

    std::shared_ptr<KVIter> it = std::make_shared<KVIter>();
    it->setCollectionId(collectionId);

    ClassTraits<T>::get(*tp, pa, it);
  }
};

template<typename T> struct PropertyStorage<T, short> : public BasePropertyStorage<T, short>{};
template<typename T> struct PropertyStorage<T, unsigned short> : public BasePropertyStorage<T, unsigned short>{};
template<typename T> struct PropertyStorage<T, int> : public BasePropertyStorage<T, int>{};
template<typename T> struct PropertyStorage<T, unsigned int> : public BasePropertyStorage<T, unsigned int>{};
template<typename T> struct PropertyStorage<T, long> : public BasePropertyStorage<T, long>{};
template<typename T> struct PropertyStorage<T, unsigned long> : public BasePropertyStorage<T, unsigned long>{};
template<typename T> struct PropertyStorage<T, long long> : public BasePropertyStorage<T, long long>{};
template<typename T> struct PropertyStorage<T, unsigned long long> : public BasePropertyStorage<T, unsigned long long>{};
template<typename T> struct PropertyStorage<T, float> : public BasePropertyStorage<T, float>{};
template<typename T> struct PropertyStorage<T, double> : public BasePropertyStorage<T, double>{};
template<typename T> struct PropertyStorage<T, bool> : public BasePropertyStorage<T, bool>{};
template<typename T> struct PropertyStorage<T, const char *> : public BasePropertyStorage<T, const char *>{};
template<typename T> struct PropertyStorage<T, std::string> : public BasePropertyStorage<T, std::string>{};

template<typename T> struct PropertyStorage<T, std::vector<short>> : public ValueVectorPropertyStorage<T, short>{};
template<typename T> struct PropertyStorage<T, std::vector<unsigned short>> : public ValueVectorPropertyStorage<T, unsigned short>{};
template<typename T> struct PropertyStorage<T, std::vector<int>> : public ValueVectorPropertyStorage<T, int>{};
template<typename T> struct PropertyStorage<T, std::vector<unsigned int>> : public ValueVectorPropertyStorage<T, unsigned int>{};
template<typename T> struct PropertyStorage<T, std::vector<long>> : public ValueVectorPropertyStorage<T, long>{};
template<typename T> struct PropertyStorage<T, std::vector<unsigned long>> : public ValueVectorPropertyStorage<T, unsigned long>{};
template<typename T> struct PropertyStorage<T, std::vector<long long>> : public ValueVectorPropertyStorage<T, long long>{};
template<typename T> struct PropertyStorage<T, std::vector<unsigned long long>> : public ValueVectorPropertyStorage<T, unsigned long long>{};
template<typename T> struct PropertyStorage<T, std::vector<float>> : public ValueVectorPropertyStorage<T, float>{};
template<typename T> struct PropertyStorage<T, std::vector<double>> : public ValueVectorPropertyStorage<T, double>{};
template<typename T> struct PropertyStorage<T, std::vector<bool>> : public ValueVectorPropertyStorage<T, bool>{};
template<typename T> struct PropertyStorage<T, std::vector<const char *>> : public ValueVectorPropertyStorage<T, const char *>{};
template<typename T> struct PropertyStorage<T, std::vector<std::string>> : public ValueVectorPropertyStorage<T, std::string>{};

template<typename T> struct PropertyStorage<T, std::set<short>> : public ValueSetPropertyStorage<T, short>{};
template<typename T> struct PropertyStorage<T, std::set<unsigned short>> : public ValueSetPropertyStorage<T, unsigned short>{};
template<typename T> struct PropertyStorage<T, std::set<int>> : public ValueSetPropertyStorage<T, int>{};
template<typename T> struct PropertyStorage<T, std::set<unsigned int>> : public ValueSetPropertyStorage<T, unsigned int>{};
template<typename T> struct PropertyStorage<T, std::set<long>> : public ValueSetPropertyStorage<T, long>{};
template<typename T> struct PropertyStorage<T, std::set<unsigned long>> : public ValueSetPropertyStorage<T, unsigned long>{};
template<typename T> struct PropertyStorage<T, std::set<long long>> : public ValueSetPropertyStorage<T, long long>{};
template<typename T> struct PropertyStorage<T, std::set<unsigned long long>> : public ValueSetPropertyStorage<T, unsigned long long>{};
template<typename T> struct PropertyStorage<T, std::set<float>> : public ValueSetPropertyStorage<T, float>{};
template<typename T> struct PropertyStorage<T, std::set<double>> : public ValueSetPropertyStorage<T, double>{};
template<typename T> struct PropertyStorage<T, std::set<bool>> : public ValueSetPropertyStorage<T, bool>{};
template<typename T> struct PropertyStorage<T, std::set<const char *>> : public ValueSetPropertyStorage<T, const char *>{};
template<typename T> struct PropertyStorage<T, std::set<std::string>> : public ValueSetPropertyStorage<T, std::string>{};

template <typename O, ObjectId O::*p>
struct ObjectIdAssign : public PropertyAssign<O, ObjectId, p> {
  ObjectIdAssign()
      : PropertyAssign<O, ObjectId, p>("objectId", new ObjectIdStorage<O>(), PropertyType(0, 0, false)) {}
};

template <typename O, typename P, std::shared_ptr<P> O::*p>
struct ObjectPropertyAssign : public PropertyAccess<O, P> {
  ObjectPropertyAssign(const char * name)
      : PropertyAccess<O, P>(name, new ObjectPropertyStorage<O, P>(), object_t<P>()) {}
  void set(O &o, P val) const override { o.*p = val;}
  P get(O &o) const override { return o.*p;}
};
template <typename O, typename P, std::shared_ptr<P> O::*p>
struct ObjectPtrPropertyAssign : public PropertyAccess<O, std::shared_ptr<P>> {
  ObjectPtrPropertyAssign(const char * name)
      : PropertyAccess<O, std::shared_ptr<P>>(name, new ObjectPtrPropertyStorage<O, P, std::shared_ptr>(), object_t<P>()) {}
  void set(O &o, std::shared_ptr<P> val) const override { o.*p = val;}
  std::shared_ptr<P> get(O &o) const override { return o.*p;}
};

template <typename O, typename P, std::vector<P> O::*p>
struct ObjectVectorPropertyAssign : public PropertyAssign<O, std::vector<P>, p> {
  ObjectVectorPropertyAssign(const char * name, bool lazy=false)
      : PropertyAssign<O, std::vector<P>, p>(name, new ObjectVectorPropertyStorage<O, P>(lazy), object_vector_t<P>()) {}
};
template <typename O, typename P, std::vector<P> O::*p>
struct ObjectVectorPropertyEmbeddedAssign : public PropertyAssign<O, std::vector<P>, p> {
  ObjectVectorPropertyEmbeddedAssign(const char * name)
      : PropertyAssign<O, std::vector<P>, p>(name, new ObjectVectorPropertyStorageEmbedded<O, P>(), object_vector_t<P>()) {}
};
template <typename O, typename P, std::vector<std::shared_ptr<P>> O::*p>
struct ObjectPtrVectorPropertyEmbeddedAssign : public PropertyAssign<O, std::vector<std::shared_ptr<P>>, p> {
  ObjectPtrVectorPropertyEmbeddedAssign(const char * name)
      : PropertyAssign<O, std::vector<std::shared_ptr<P>>, p>(name, new ObjectPtrVectorPropertyStorageEmbedded<O, P>(), object_vector_t<P>()) {}
};
template <typename O, typename P, std::vector<std::shared_ptr<P>> O::*p>
struct ObjectPtrVectorPropertyAssign : public PropertyAssign<O, std::vector<std::shared_ptr<P>>, p> {
  ObjectPtrVectorPropertyAssign(const char * name, bool lazy=false)
      : PropertyAssign<O, std::vector<std::shared_ptr<P>>, p>(name, new ObjectPtrVectorPropertyStorage<O, P>(lazy), object_vector_t<P>()) {}
};
template <typename O, typename P, typename KVIter, typename Iter, std::shared_ptr<Iter> O::*p>
struct ObjectIterPropertyAssign : public PropertyAssign<O, std::shared_ptr<Iter>, p> {
  ObjectIterPropertyAssign(const char * name)
      : PropertyAssign<O, std::shared_ptr<Iter>, p>(name, new ObjectIterPropertyStorage<O, P, KVIter, Iter>(), object_vector_t<P>()) {}
};

} //kv
} //persistence
} //flexis

#endif //FLEXIS_FLEXIS_KVSTORE_H
