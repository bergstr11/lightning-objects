//
// Created by cse on 10/9/15.
//

#ifndef FLEXIS_FLEXIS_KVTRAITS_H
#define FLEXIS_FLEXIS_KVTRAITS_H

#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <cstring>
#include <typeinfo>
#include <stdexcept>
#include <stdint.h>
#include <type_traits>
#include "kvbuf.h"
#include "FlexisPersistence_Export.h"

namespace flexis {
namespace persistence {

namespace kv {

#define ARRAY_SZ(x) unsigned(sizeof(x) / sizeof(decltype(*x)))

class invalid_pointer_error : public persistence_error
{
public:
  invalid_pointer_error() : persistence_error("invalid pointer argument: not created by KV store", "") {}
};
class invalid_classid_error : public persistence_error
{
  std::string mk(const char *msg, ClassId cid) {
    std::stringstream ss;
    ss << msg << cid;
    return ss.str();
  }
public:
  invalid_classid_error(ClassId cid) : persistence_error(mk("invalid classid: ", cid), "is class registered?") {}
};

struct PropertyType
{
  //predefined base type id, irrelevant if className is set
  const ClassId id;

  //it's a vector
  const bool isVector;

  //number of bytes, 0 if variable size (e.g. string). For a vector, this is the byteSize of the elements
  const unsigned byteSize;

  //name of the mapped type if this is an object type
  const char *className;

  PropertyType(unsigned id, unsigned byteSize, bool isVector=false)
      : id(id), isVector(isVector), byteSize(byteSize), className(nullptr) {}
  PropertyType(const char *clsName, bool isVector=false) :
      id(0), isVector(isVector), byteSize(ObjectKey_sz), className(clsName) {}

  bool operator == (const PropertyType &other) const {
    return id == other.id
           && isVector == other.isVector
           && className == other.className;
  }
};

template <typename T> struct TypeTraits;
#define TYPETRAITS template <> struct TypeTraits
#define TYPETRAITSV template <> struct TypeTraits<std::vector
#define TYPETRAITSS template <> struct TypeTraits<std::set

#define TYPEDEF(_id, _sz) static const ClassId id=_id; static const unsigned byteSize=_sz; static const bool isVect=false;
#define TYPEDEFV(_id, _sz) static const ClassId id=_id; static const unsigned byteSize=_sz; static const bool isVect=true;

TYPETRAITS<short>             {TYPEDEF(1, 2);};
TYPETRAITS<unsigned short>    {TYPEDEF(2, 2);};
TYPETRAITS<int>               {TYPEDEF(3, 4);};
TYPETRAITS<unsigned int>      {TYPEDEF(4, 4);};
TYPETRAITS<long>              {TYPEDEF(5, 8);};
TYPETRAITS<unsigned long>     {TYPEDEF(6, 8);};
TYPETRAITS<long long>         {TYPEDEF(7, 8);};
TYPETRAITS<unsigned long long>{TYPEDEF(8, 8);};
TYPETRAITS<bool>              {TYPEDEF(9, 1);};
TYPETRAITS<float>             {TYPEDEF(10, 4);};
TYPETRAITS<double>            {TYPEDEF(11, 8);};
TYPETRAITS<const char *>      {TYPEDEF(12, 0);};
TYPETRAITS<std::string>       {TYPEDEF(13, 0);};

TYPETRAITSV<short>>             {TYPEDEFV(1, 2);};
TYPETRAITSV<unsigned short>>    {TYPEDEFV(2, 2);};
TYPETRAITSV<int>>               {TYPEDEFV(3, 4);};
TYPETRAITSV<unsigned int>>      {TYPEDEFV(4, 4);};
TYPETRAITSV<long>>              {TYPEDEFV(5, 8);};
TYPETRAITSV<unsigned long>>     {TYPEDEFV(6, 8);};
TYPETRAITSV<long long>>         {TYPEDEFV(7, 8);};
TYPETRAITSV<unsigned long long>>{TYPEDEFV(8, 8);};
TYPETRAITSV<bool>>              {TYPEDEFV(9, 1);};
TYPETRAITSV<float>>             {TYPEDEFV(10, 4);};
TYPETRAITSV<double>>            {TYPEDEFV(11, 8);};
TYPETRAITSV<const char *>>      {TYPEDEFV(12, 0);};
TYPETRAITSV<std::string>>       {TYPEDEFV(13, 0);};

TYPETRAITSS<short>>             {TYPEDEFV(1, 2);};
TYPETRAITSS<unsigned short>>    {TYPEDEFV(2, 2);};
TYPETRAITSS<int>>               {TYPEDEFV(3, 4);};
TYPETRAITSS<unsigned int>>      {TYPEDEFV(4, 4);};
TYPETRAITSS<long>>              {TYPEDEFV(5, 8);};
TYPETRAITSS<unsigned long>>     {TYPEDEFV(6, 8);};
TYPETRAITSS<long long>>         {TYPEDEFV(7, 8);};
TYPETRAITSS<unsigned long long>>{TYPEDEFV(8, 8);};
TYPETRAITSS<bool>>              {TYPEDEFV(9, 1);};
TYPETRAITSS<float>>             {TYPEDEFV(10, 4);};
TYPETRAITSS<double>>            {TYPEDEFV(11, 8);};
TYPETRAITSS<const char *>>      {TYPEDEFV(12, 0);};
TYPETRAITSS<std::string>>       {TYPEDEFV(13, 0);};

//these assertions must hold because certain elmements are written/read natively
static_assert(sizeof(ClassId) == TypeTraits<ClassId>::byteSize, "ClassId: byteSize must match native size");
static_assert(sizeof(ObjectId) == TypeTraits<ObjectId>::byteSize, "ObjectId: byteSize must match native size");
static_assert(sizeof(PropertyId) == TypeTraits<PropertyId>::byteSize, "PropertyId: byteSize must match native size");
static_assert(sizeof(size_t) == TypeTraits<size_t>::byteSize, "size_t: byteSize must match native size");

class ReadTransaction;
class WriteTransaction;
class PropertyAccessBase;
class ObjectBuf;

enum class StoreMode {force_none, force_all, force_buffer, force_property};

enum class StoreLayout {all_embedded, embedded_key, property, none};

/**
 * abstract superclass for classes that handle serializing mapped values to the datastore
 */
struct StoreAccessBase
{
  const StoreLayout layout;
  size_t fixedSize;

  StoreAccessBase(StoreLayout layout=StoreLayout::all_embedded, size_t fixedSize=0)
      : layout(layout), fixedSize(fixedSize) {}

  /**
   * initialize the fixed size. Subclasses that need to calculate fixed size at schema initialization
   * should override
   *
   * @return the ready initialized fixed size.
   */
  virtual size_t initFixedSize() {return fixedSize;}

  /**
   * determine whether this storage participates in update/delete preparation
   *
   * @return whether this mapping participates in update/delete preparation
   */
  virtual bool preparesUpdates(ClassId classId) {return false;}

  /**
   * determine the size from a serialized buffer. The buffer's read position is at the start of this object's data
   *
   * @return the size of this object
   */
  virtual size_t size(ObjectBuf &buf) const = 0;

  /**
   * determine the size from a live object
   *
   * @return the buffer size required to save the given property value
   */
  virtual size_t size(void *obj, const PropertyAccessBase *pa) {return 0;}

  /**
   * prepare an update for the given object property
   *
   * @param buf the object data as currently saved
   * @param obj the object about to be saved
   * @param pa the property about to be saved
   * @return the same as size(buf)
   */
  virtual size_t prepareUpdate(ObjectBuf &buf, void *obj, const PropertyAccessBase *pa) {
    return size(buf);
  }

  /**
   * prepare a delete for the given object property
   *
   * @param tr the write transaction
   * @param buf the object data as currently saved
   * @param obj the object about to be deleted
   * @param pa the property represented by this storage
   * @return the same as size(buf)
   */
  virtual size_t prepareDelete(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa) {
    return size(buf);
  }

  virtual void save(WriteTransaction *tr,
                    ClassId classId, ObjectId objectId,
                    void *obj, const PropertyAccessBase *pa,
                    StoreMode mode=StoreMode::force_none) = 0;

  virtual void load(ReadTransaction *tr,
                    ReadBuf &buf,
                    ClassId classId, ObjectId objectId,
                    void *obj, const PropertyAccessBase *pa,
                    StoreMode mode=StoreMode::force_none) = 0;

  virtual void * initMember(void *obj, const PropertyAccessBase *pa) {
    return nullptr;
  }
};

/**
 * abstract superclass for all Store Access classes that represent mapped-object valued properties where
 * the referred-to object is saved individually and key value saved in the enclosing object's buffer
 */
struct StoreAccessEmbeddedKey : public StoreAccessBase
{
  StoreAccessEmbeddedKey() : StoreAccessBase(StoreLayout::embedded_key, ObjectKey_sz) {}

  size_t size(ObjectBuf &buf) const override {return ObjectKey_sz;}
  size_t size(void *obj, const PropertyAccessBase *pa) override {return ObjectKey_sz;}
};

/**
 * abstract superclass for all Store Access classes that represent mapped-object valued properties that are saved
 * under a property key, with nothing saved in the enclosing object's buffer
 */
struct StoreAccessPropertyKey: public StoreAccessBase
{
  StoreAccessPropertyKey() : StoreAccessBase(StoreLayout::property) {}

  size_t size(ObjectBuf &buf) const override {return 0;}
  size_t size(void *obj, const PropertyAccessBase *pa) override {return 0;}
};

template<typename T, typename V, unsigned byteSize=TypeTraits<V>::byteSize> struct PropertyStorage
    : public StoreAccessBase {
  PropertyStorage() : StoreAccessBase(byteSize) {}
};

/**
 * base class for value handler templates.
 */
template <bool Fixed>
struct ValueTraitsBase
{
  const bool fixed;
  ValueTraitsBase() : fixed(Fixed) {}
};

/**
 * base class for single-byte value handlers
 */
template <typename T>
struct ValueTraitsByte : public ValueTraitsBase<true>
{
  static size_t size(const T &val) {
    return 1;
  }
  static void getBytes(ReadBuf &buf, T &val) {
    const byte_t *data = buf.read(1);
    val = (T)*data;
  }
  static void putBytes(WriteBuf &buf, T val) {
    byte_t *data = buf.allocate(1);
    *data = (byte_t)val;
  }
};

/**
 * base template value handler for fixed size values
 */
template <typename T>
struct ValueTraits : public ValueTraitsBase<true>
{
  static size_t size(const T &val) {
    return TypeTraits<T>::byteSize;
  }
  static void getBytes(ReadBuf &buf, T &val) {
    size_t byteSize = TypeTraits<T>::byteSize;
    const byte_t *data = buf.read(byteSize);
    val = read_integer<T>(data, byteSize);
  }
  static void putBytes(WriteBuf &buf, T &val) {
    size_t byteSize = TypeTraits<T>::byteSize;
    byte_t *data = buf.allocate(byteSize);
    write_integer(data, val, byteSize);
  }
};

/**
 * value handler specialization for boolean values
 */
template <>
struct ValueTraits<bool> : public ValueTraitsBase<true>
{
  static size_t size(const bool &val) {
    return TypeTraits<bool>::byteSize;
  }
  static void getBytes(ReadBuf &buf, bool &val) {
    const byte_t *data = buf.read(1);
    val = *data != 0;
  }
  static void putBytes(WriteBuf &buf, bool val) {
    byte_t *data = buf.allocate(1);
    *data = byte_t(val ? 1 : 0);
  }
};

/**
 * value handler specialization for string values
 */
template <>
struct ValueTraits<std::string> : public ValueTraitsBase<false>
{
  static size_t size(const std::string &val) {
    return val.length() + 1;
  }
  static void getBytes(ReadBuf &buf, std::string &val) {
    val = (const char *)buf.read(0);
    buf.read(val.length() +1); //move the pointer
  }
  static void putBytes(WriteBuf &buf, std::string &val) {
    buf.append(val.data(), val.length()+1);
  }
};

/**
 * value handler specialization for C string values
 */
template <>
struct ValueTraits<const char *> : public ValueTraitsBase<false>
{
  static size_t size(const char * const &val) {
    return strlen(val) + 1;
  }
  static void getBytes(ReadBuf &buf, const char *&val) {
    val = buf.readCString();
  }
  static void putBytes(WriteBuf &buf, const char *val) {
    buf.appendCString(val);
  }
};

/**
 * value handler base class for float values
 */
template <typename T>
struct ValueTraitsFloat : public ValueTraitsBase<true>
{
  static size_t size(const T &val) {
    return TypeTraits<T>::byteSize;
  }
  static void getBytes(ReadBuf &buf, T &val) {
    size_t byteSize = TypeTraits<T>::byteSize;
    const byte_t *data = buf.read(byteSize);
    val = *reinterpret_cast<const T *>(data);
  }
  static void putBytes(WriteBuf &buf, T val) {
    size_t byteSize = TypeTraits<T>::byteSize;
    byte_t *data = buf.allocate(byteSize);
    *reinterpret_cast<T *>(data) = val;
  }
};

#define PROPERTY_TYPE(P) PropertyType(TypeTraits<P>::id, TypeTraits<P>::byteSize, TypeTraits<P>::isVect)

/**
 * value handler specialization for float values
 */
template <>
struct ValueTraits<float> : public ValueTraitsFloat<float> {};
/**
 * value handler specialization for double values
 */
template <>
struct ValueTraits<double> : public ValueTraitsFloat<double> {};

class Properties;

/**
 * non-templated base class for property accessors
 */
struct PropertyAccessBase
{
  const char * const name;
  bool enabled = true;
  ClassId classId;
  PropertyId id = 0;
  StoreAccessBase *storage;
  const PropertyType type;

  PropertyAccessBase(const char * name, StoreAccessBase *storage, const PropertyType &type)
      : name(name), storage(storage), type(type) {}

  virtual bool same(void *obj, ObjectId oid) {return false;}
  virtual ~PropertyAccessBase() {delete storage;}

  void *initMember(void *obj) const {
    return storage->initMember(obj, this);
  }

  virtual void setup(Properties *props) const {}
};

/**
 * templated abstract superclass for property accessors
 */
template <typename O, typename P>
struct PropertyAccess : public PropertyAccessBase {
  PropertyAccess(const char * name, StoreAccessBase *storage, const PropertyType &type)
      : PropertyAccessBase(name, storage, type) {}
  virtual void set(O &o, P val) const = 0;
  virtual P get(O &o) const = 0;
};

/**
 * property accessor that performs direct assignment
 */
template <typename O, typename P, P O::*p> struct PropertyAssign : public PropertyAccess<O, P> {
  PropertyAssign(const char * name, StoreAccessBase *storage, const PropertyType &type)
      : PropertyAccess<O, P>(name, storage, type) {}
  void set(O &o, P val) const override { o.*p = val;}
  P get(O &o) const override { return o.*p;}
};

/**
 * assignment property accessor for predeclared base types
 */
template <typename O, typename P, P O::*p>
struct BasePropertyAssign : public PropertyAssign<O, P, p> {
  BasePropertyAssign(const char * name)
      : PropertyAssign<O, P, p>(name, new PropertyStorage<O, P>(), PROPERTY_TYPE(P)) {}
};

template <typename T> struct ClassTraits;

/**
 * dummy class
 */
struct EmptyClass
{
};

/**
 * iterates over class property mappings. In an inheritance context, the iteration will start with the topmost
 * class and  run down the hierarchy so that all properties are covered. Single-inheritance only
 */
class Properties
{
protected:
  const PropertyAccessBase * keyProperty = nullptr;
  const unsigned numProps;
  const PropertyAccessBase *** const decl_props;
  Properties * superIter = nullptr;
  unsigned startPos = 0;

  Properties(const PropertyAccessBase ** decl_props[], unsigned numProps)
      : decl_props(decl_props), numProps(numProps), fixedSize(0)
  {}

  Properties(const Properties& mit) = delete;
public:
  size_t fixedSize;

  virtual void init() = 0;

  template <typename O>
  PropertyAccess<O, ObjectId> *objectIdAccess()
  {
    if(keyProperty)
      return (PropertyAccess<O, ObjectId> *)keyProperty;
    else
      return superIter ? superIter->objectIdAccess<O>() : nullptr;
  }

  bool preparesUpdates(ClassId classId) {
    for(int i=0; i<numProps; i++)
      if((*decl_props[i])->storage->preparesUpdates(classId))
        return true;
    return false;
  }

  inline unsigned full_size() {
    return superIter ? superIter->full_size() + numProps : numProps;
  }

  const PropertyAccessBase * get(unsigned index) {
    return index >= startPos ? *decl_props[index-startPos] : superIter->get(index);
  }

  void setKeyProperty(const PropertyAccessBase *prop) {
    keyProperty = prop;
  }
};

template <typename S>
class PropertiesImpl : public Properties
{
  PropertiesImpl(const PropertyAccessBase ** decl_props[], unsigned numProps)
      : Properties(decl_props, numProps)
  {
    for(unsigned i=0; i<numProps; i++) {
      const PropertyAccessBase *pa = *decl_props[i];
      pa->setup(this);
    }
  }
public:
  template <typename T>
  static Properties *mk()
  {
    Properties *p = new PropertiesImpl<S>(
        ClassTraits<T>::decl_props,
        ClassTraits<T>::num_decl_props);

    //assign consecutive IDs, starting at 2 (0 and 1 are reserved)
    for(unsigned i=0; i<p->full_size(); i++)
      const_cast<PropertyAccessBase *>(p->get(i))->id = i+2;

    return p;
  }

  void init() override
  {
    //determine superclass and property start position
    superIter = ClassTraits<S>::traits_properties;
    startPos = superIter ? superIter->full_size() : 0;

    //see if we're fixed size
    fixedSize = 0;
    if(superIter) {
      fixedSize = superIter->fixedSize;
      if(!fixedSize) return;
    }
    for(unsigned i=0; i<numProps; i++) {
      const PropertyAccessBase *pa = *decl_props[i];
      if(pa->enabled) {
        switch(pa->storage->layout) {
          case StoreLayout::all_embedded: {
            if (!pa->storage->initFixedSize()) {
              fixedSize = 0;
              return;
            }
            fixedSize += pa->storage->fixedSize;
            break;
          }
          case StoreLayout::embedded_key:
            fixedSize += ObjectKey_sz;
            break;
          case StoreLayout::property:
            break;
        }
      }
    }
  }
};

enum class SchemaCompatibility {write, read, none};

/**
 * non-templated superclass for ClassInfo
 */
struct AbstractClassInfo {
  static const ClassId MIN_USER_CLSID = 10; //ids below are reserved

  AbstractClassInfo(const AbstractClassInfo &other) = delete;

  SchemaCompatibility compatibility = SchemaCompatibility::write;

  const char *name;
  const std::type_info &typeinfo;
  ClassId classId = 0;
  ObjectId maxObjectId = 0;
  bool refcounting = false;

  std::set<ClassId> prepareClasses;
  std::vector<AbstractClassInfo *> subs;

  AbstractClassInfo(const char *name, const std::type_info &typeinfo, ClassId classId)
      : name(name), typeinfo(typeinfo), classId(classId) {}

  void addSub(AbstractClassInfo *rsub) {
    subs.push_back(rsub);
  }

  bool isPoly() {
    return !subs.empty();
  }

  bool hasClassId(ClassId cid) {
    if(classId == cid) return true;
    for(auto &sub : subs)
      if(sub->hasClassId(cid)) return true;
    return false;
  }

  void setRefCounting(bool refcount) {
    refcounting = refcount;
    for(auto &sub : subs) {
      sub->setRefCounting(refcount);
    }
  }

  bool isInstance(ClassId _classId) {
    if(classId == _classId) return true;
    for(auto s : subs) {
      if(s->isInstance(_classId)) return true;
    }
    return false;
  }

  AbstractClassInfo *resolve(ClassId otherClassId)
  {
    if(otherClassId == classId) {
      return this;
    }
    for(auto res : subs) {
      AbstractClassInfo *r = res->resolve(otherClassId);
      if(r) return r;
    }
    return nullptr;
  }

  AbstractClassInfo *resolve(const std::type_info &ti)
  {
      const char *n1 = ti.name();
      const char *n2 = typeinfo.name();
    if(ti == typeinfo) {
      return this;
    }
    for(auto res : subs) {
      AbstractClassInfo *r = res->resolve(ti);
      if(r) return r;
    }
    return nullptr;
  }

  /**
   * search the inheritance tree rooted in this object for a class info with the given classId
   * @return the classinfo
   * @throw persistence_error if not found
   */
  AbstractClassInfo *doresolve(ClassId otherClassId)
  {
    AbstractClassInfo *resolved = resolve(otherClassId);
    if(!resolved) {
      throw persistence_error("unknow classId. Class missing from registry");
    }
    return resolved;
  }

  /**
   * search the inheritance tree rooted in this object for a class info with the given typeid
   * @return the classinfo
   * @throw persistence_error if not found
   */
  AbstractClassInfo *doresolve(const std::type_info &ti)
  {
    AbstractClassInfo *resolved = resolve(ti);
    if(!resolved) {
      throw persistence_error("unknow typeid. Class missing from registry");
    }
    return resolved;
  }

  std::vector<ClassId> allClassIds() {
    std::vector<ClassId> ids;
    addClassIds(ids);
    return ids;
  }
  void addClassIds(std::vector<ClassId> &ids) {
    ids.push_back(classId);
    for(auto &sub : subs) sub->addClassIds(ids);
  }
};

namespace sub {

/**
 * a group of structs that resolve the variadic template list used by the ClassInfo#subclass function
 * the list is expanded and the ClassTraits for each type are notified about the subclass
 */

//this one does the real work by adding S as subtype to T
template<typename T, typename S>
struct resolve_impl
{
  bool publish(AbstractClassInfo *res) {
    ClassTraits<S>::traits_info->addSub(res);
    return true;
  }
};

//primary template
template<typename T, typename... Sargs>
struct resolve;

//helper that removes one type arg from the list
template<typename T, typename S, typename... Sargs>
struct resolve_helper
{
  bool publish(AbstractClassInfo *res) {
    if(resolve_impl<T, S>().publish(res)) return true;
    return resolve<T, Sargs...>().publish(res);
  }
};

//template specialization for non-empty list
template<typename T, typename... Sargs>
struct resolve
{
  bool publish(AbstractClassInfo *res) {
    return resolve_helper<T, Sargs...>().publish(res);
  }
};

//template specialization for empty list
template<typename T>
struct resolve<T>
{
  bool publish(AbstractClassInfo *res) {return false;}
};

template <typename T>
struct Substitute {
  virtual T *getPtr() = 0;
};

template <typename T, typename S>
struct SubstituteImpl : public Substitute<T> {
  T *getPtr() override {return new S();}
};

} //sub

/**
 * peer object for the ClassTraitsBase below which contains class metadata and references subclasses
 */
template <typename T, typename ... Sup>
struct ClassInfo : public AbstractClassInfo
{
  T *(* const getSubstitute)();
  size_t (* const size)(T *obj);
  void * (* const initMember)(T *obj, const PropertyAccessBase *pa);
  T * (* const makeObject)(ClassId classId);
  Properties * (* const getProperties)(ClassId classId);
  bool (* const addSize)(T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags);
  bool (* const get_objectkey)(const std::shared_ptr<T> &obj, ObjectKey *&key, unsigned flags);
  bool (* const prep_delete)(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa, size_t &size, unsigned flags);
  bool (* const prep_update)(ObjectBuf &buf, T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags);
  bool (* const save)(WriteTransaction *wtr,
                      ClassId classId, ObjectId objectId, T *obj, const PropertyAccessBase *pa, StoreMode mode, unsigned flags);
  bool (* const load)(ReadTransaction *tr, ReadBuf &buf,
                      ClassId classId, ObjectId objectId, T *obj, const PropertyAccessBase *pa, StoreMode mode, unsigned flags);

  sub::Substitute<T> *substitute = nullptr;

  ClassInfo(const char *name, const std::type_info &typeinfo, ClassId classId=MIN_USER_CLSID)
      : AbstractClassInfo(name, typeinfo, classId),
        getSubstitute(&ClassTraits<T>::getSubstitute),
        size(&ClassTraits<T>::size),
        initMember(&ClassTraits<T>::initMember),
        makeObject(&ClassTraits<T>::makeObject),
        getProperties(&ClassTraits<T>::getProperties),
        addSize(&ClassTraits<T>::addSize),
        get_objectkey(&ClassTraits<T>::get_objectkey),
        prep_delete(&ClassTraits<T>::prep_delete),
        prep_update(&ClassTraits<T>::prep_update),
        save(&ClassTraits<T>::save),
        load(&ClassTraits<T>::load) {}

  ~ClassInfo() {if(substitute) delete substitute;}

  template <typename S>
  void setSubstitute() {
    substitute = new sub::SubstituteImpl<T, S>();
  }

  template <typename ... Sup2>
  static ClassInfo<T, Sup...> *subclass(const char *name, const std::type_info &typeinfo, ClassId classId=MIN_USER_CLSID)
  {
    //create a classinfo
    return new ClassInfo<T, Sup2...>(name, typeinfo, classId);
  }

  void publish() {
    //make it known to superclasses
    sub::resolve<T, Sup...>().publish(this);
  }
};

#define FIND_CLS(__Tpl, __cid) static_cast<ClassInfo<__Tpl> *>(ClassTraits<__Tpl>::traits_info->resolve(__cid))
#define RESOLVE_SUB(__cid) static_cast<ClassInfo<T> *>(ClassTraits<T>::traits_info->doresolve(__cid))
#define RESOLVE_SUB_TI(__ti) static_cast<ClassInfo<T> *>(ClassTraits<T>::traits_info->doresolve(__ti))

/**
 * base class for class/inheritance resolution infrastructure. Every mapped class is represented by a templated
 * subclass of this class. All calls to access/update mapped object properties should go through here and will be
 * dispatched to the correct location. The correct location is determined by the classId which is uniquely assigned
 * to each mapped class. Many calls here will first determine the correct ClassTraits instance, and from there
 * hand over to non-templated API's, like PropertyAccessBase's API. This ensures that the cast operations (at handover
 * to the non-templated API and thereafter before actual processing) happen on the exact type level, i.e., at handover,
 * the ClassTraits template parameter type T is always the exact type of the handed-over object. Thus the T/void/T
 * cast sequence is without issues.
 *
 * Heres an illustration:
 *
 * - mapped type hierarchy: S <- T (T subclasses S)
 * - persistent operation is exectuted with template parameter type S and an operand of type T
 * - the operation needs to hand over to a property mapping (PropertyAccessBase), which is not templated and therefore
 *   takes (void *). Let the property be defined in ClassTraits<T>, while the operation uses ClassTraits<S>
 * - inside PropertyAccessBase, the pointer is cast back to T to perform actual processing
 * ==> this does not work. The cast S* -> void* -> T* destroys the object, because the compiler cannot support it.
 * ==> we therefore perform lookup by classId to determine the exact match, which is ClassTraits<T>.
 * ==> the cast T* -> void* -> T* is harmless
 */
template <typename T, typename SUP=EmptyClass>
class ClassTraitsBase
{
  template <typename, typename ...> friend struct ClassInfo;

  static const unsigned FLAG_UP = 0x1;
  static const unsigned FLAG_DN = 0x2;
  static const unsigned FLAG_HR = 0x4;
  static const unsigned FLAGS_ALL = FLAG_UP | FLAG_DN | FLAG_HR;

#define DN flags & FLAG_DN
#define UP flags & FLAG_UP

  /**
   * determine the buffer size for the given object. Non-polymorpic
   */
  static size_t size(T *obj)
  {
    if(traits_properties->fixedSize) return traits_properties->fixedSize;

    size_t size = 0;
    for(unsigned i=0, sz=traits_properties->full_size(); i<sz; i++) {
      auto pa = traits_properties->get(i);

      if(!pa->enabled) continue;

      addSize(obj, pa, size);
    }
    return size;
  }

public:
  static bool traits_initialized;
  static const char *traits_classname;
  static ClassInfo<T, SUP> *traits_info;
  static Properties * traits_properties;
  static const PropertyAccessBase ** decl_props[];
  static const unsigned num_decl_props;

  /**
   * perform lazy initialization of static structures (only once).
   */
  static void init() {
    if(!traits_initialized) {
      traits_initialized = true;

      traits_properties->init();
      traits_info->publish();
    }
  }

  /**
   * @return the objectid accessor for this class
   */
  static PropertyAccess<T, ObjectId> *objectIdAccess() {
    return traits_properties->objectIdAccess<T>();
  }

  static size_t bufferSize(T *obj, ClassId *clsId=nullptr)
  {
    const std::type_info &ti = typeid(*obj);
    if(ti == traits_info->typeinfo) {
      if(clsId) *clsId = traits_info->classId;
      return size(obj);
    }
    else {
      ClassInfo<T> *sub = RESOLVE_SUB_TI(ti);
      if(clsId) *clsId = sub->classId;
      return sub->size(obj);
    }
  }

  static bool needsPrepare() {
    return !traits_info->prepareClasses.empty();
  }

  static T *getSubstitute()
  {
    if(traits_info->substitute) return traits_info->substitute->getPtr();

    for(auto &sub : traits_info->subs) {
      ClassInfo<T> * si = static_cast<ClassInfo<T> *>(sub);
      T *subst = si->getSubstitute();
      if(subst != nullptr) return subst;
    }
    return nullptr;
  }

  static void * initMember(T *obj, const PropertyAccessBase *pa)
  {
    if(pa->classId == traits_info->classId)
      return pa->initMember(obj);
    else if(pa->classId)
      return RESOLVE_SUB(pa->classId)->initMember(obj, pa);
    return nullptr;
  }

  static Properties * getProperties(ClassId classId)
  {
    if(classId == traits_info->classId)
      return traits_properties;
    else if(classId)
      return RESOLVE_SUB(classId)->getProperties(classId);
    return nullptr;
  }

  static bool addSize(T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags=FLAGS_ALL)
  {
    if(pa->classId == traits_info->classId) {
      size += pa->storage->size(obj, pa);
      return true;
    }
    else if(pa->classId) {
      if(UP && ClassTraits<SUP>::addSize(obj, pa, size, FLAG_UP))
        return true;
      return DN && RESOLVE_SUB(pa->classId)->addSize(obj, pa, size, FLAG_DN);
    }
    return false;
  }

  static ObjectKey *getObjectKey(const std::shared_ptr<T> &obj, bool force=true)
  {
    ObjectKey *key = nullptr;
    if(!get_objectkey(obj, key) && force) throw invalid_pointer_error();
    return key;
  }

  static bool get_objectkey(const std::shared_ptr<T> &obj, ObjectKey *&key, unsigned flags=FLAGS_ALL)
  {
    object_handler<T> *handler = std::get_deleter<object_handler<T>>(obj);
    if(handler) {
      key = handler;
      return true;
    }
    else {
      if(UP && ClassTraits<SUP>::get_objectkey(obj, key, FLAG_UP)) return true;
      if(DN) {
        for(auto &sub : traits_info->subs) {
          ClassInfo<T> * si = static_cast<ClassInfo<T> *>(sub);
          if(si->get_objectkey(obj, key, FLAG_DN)) return true;
        }
      }
    }
    return false;
  }

  static size_t prepareUpdate(ObjectBuf &buf, T *obj, const PropertyAccessBase *pa)
  {
    size_t size;
    if(!prep_update(buf, obj, pa, size)) throw invalid_classid_error(pa->classId);
    return size;
  }
  static bool prep_update(ObjectBuf &buf, T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags=FLAGS_ALL)
  {
    if(pa->classId == traits_info->classId) {
      size = pa->storage->prepareUpdate(buf, obj, pa);
      return true;
    }
    else if(pa->classId) {
      if(UP && ClassTraits<SUP>::prep_update(buf, obj, pa, size, FLAG_UP))
        return true;
      return DN && RESOLVE_SUB(pa->classId)->prep_update(buf, obj, pa, size, FLAG_DN);
    }
    return false;
  }

  static size_t prepareDelete(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa)
  {
    size_t size;
    if(!prep_delete(tr, buf, pa, size)) throw invalid_classid_error(pa->classId);
    return size;
  }
  static bool prep_delete(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa, size_t &size, unsigned flags=FLAGS_ALL)
  {
    if(pa->classId == traits_info->classId) {
      size = pa->storage->prepareDelete(tr, buf, pa);
      return true;
    }
    else if(pa->classId) {
      if(UP && ClassTraits<SUP>::prep_delete(tr, buf, pa, size, FLAG_UP))
        return true;
      return DN && RESOLVE_SUB(pa->classId)->prep_delete(tr, buf, pa, size, FLAG_DN);
    }
    return false;
  }

  static bool save(WriteTransaction *wtr,
                   ClassId classId, ObjectId objectId, T *obj, const PropertyAccessBase *pa, StoreMode mode, unsigned flags=FLAGS_ALL)
  {
    if(pa->classId == traits_info->classId) {
      pa->storage->save(wtr, classId, objectId, obj, pa, mode);
      return true;
    }
    else if(pa->classId) {
      if(UP && ClassTraits<SUP>::save(wtr, classId, objectId, obj, pa, mode, FLAG_UP))
        return true;
      return DN && RESOLVE_SUB(pa->classId)->save(wtr, classId, objectId, obj, pa, mode, FLAG_DN);
    }
    return false;
  }

  static bool load(ReadTransaction *tr,
                   ReadBuf &buf, ClassId classId, ObjectId objectId, T *obj, const PropertyAccessBase *pa,
                   StoreMode mode=StoreMode::force_none, unsigned flags=FLAGS_ALL)
  {
    if(pa->classId == traits_info->classId) {
      pa->storage->load(tr, buf, classId, objectId, obj, pa, mode);
      return true;
    }
    else if(pa->classId) {
      if(UP && ClassTraits<SUP>::load(tr, buf, classId, objectId, obj, pa, mode, FLAG_UP))
        return true;
      return DN && RESOLVE_SUB(pa->classId)->load(tr, buf, classId, objectId, obj, pa, mode, FLAG_DN);
    }
    return false;
  }

  template <typename TV>
  static void put(T &d, const PropertyAccessBase *pa, TV &value, unsigned flags=FLAGS_ALL) {
    if(pa->classId != traits_info->classId)
      throw persistence_error("internal error: type mismatch");

    const PropertyAccess <T, TV> *acc = (const PropertyAccess <T, TV> *) pa;
    value = acc->get(d);
  }

  /**
   * update the given property using value. Must only be called after type resolution, such
   * that pa->classId == info->classId
   */
  template <typename TV>
  static void get(T &d, const PropertyAccessBase *pa, TV &value, unsigned flags=FLAGS_ALL) {
    if(pa->classId != traits_info->classId)
      throw persistence_error("internal error: type mismatch");

    const PropertyAccess<T, TV> *acc = (const PropertyAccess<T, TV> *)pa;
    acc->set(d, value);
  }
};

/**
 * ClassTraits extension for concrete classes
 */
template <typename T> struct ClassTraitsAbstract
{
  static const bool isAbstract = true;

  static T *makeObject(ClassId classId)
  {
    if(classId == ClassTraits<T>::traits_info->classId) {
      throw persistence_error("abstract class cannot be instantiated");
    }
    else if(classId)
      return RESOLVE_SUB(classId)->makeObject(classId);
    return nullptr;
  }
};

/**
 * ClassTraits extension for abstract classes
 */
template <typename T> struct ClassTraitsConcrete
{
  static const bool isAbstract = false;

  static T *makeObject(ClassId classId)
  {
    if(classId == ClassTraits<T>::traits_info->classId) {
      return new T();
    }
    else if(classId)
      return RESOLVE_SUB(classId)->makeObject(classId);
    return nullptr;
  }
};

/**
 * represents a non-class, e.g. where a mapped superclass must be defined but does not exist
 */
template <>
struct ClassTraits<EmptyClass>
{
  static const bool isAbstract = true;
  static ClassInfo<EmptyClass> *traits_info;
  static Properties * traits_properties;
  static const unsigned num_decl_props = 0;
  static const PropertyAccessBase ** decl_props[0];

  static EmptyClass *makeObject(ClassId classId) {return nullptr;}
  static Properties * getProperties(ClassId classId) {return nullptr;}

  static void init() {}

  template <typename T>
  static T *getSubstitute() {
    return nullptr;
  }
  template <typename T>
  static size_t bufferSize(T *obj, ClassId *cid=nullptr) {
    return 0;
  }
  static bool needsPrepare() {
    return false;
  }
  template <typename T>
  static size_t size(T *obj) {
    return 0;
  }
  template <typename T>
  static PropertyAccess<T, ObjectId> *objectIdAccess() {
    return nullptr;
  }
  template <typename T>
  static void * initMember(T *obj, const PropertyAccessBase *pa) {
    return nullptr;
  }
  template <typename T>
  static bool addSize(T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags=0) {
    return false;
  }
  template <typename T>
  static ObjectKey *getObjectKey(const std::shared_ptr<T> &obj, bool force=true) {
    return nullptr;
  }
  template <typename T>
  static bool get_objectkey(const std::shared_ptr<T> &obj, ObjectKey *&key, unsigned flags) {
    return false;
  }
  static size_t prepareDelete(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa) {
    return 0;
  }
  static bool prep_delete(WriteTransaction *tr, ObjectBuf &buf, const PropertyAccessBase *pa, size_t &size, unsigned flags=0) {
    return false;
  }
  template <typename T>
  static size_t prepareUpdate(ObjectBuf &buf, T *obj, const PropertyAccessBase *pa) {
    return 0;
  }
  template <typename T>
  static bool prep_update(ObjectBuf &buf, T *obj, const PropertyAccessBase *pa, size_t &size, unsigned flags=0) {
    return false;
  }
  template <typename T>
  static bool save(WriteTransaction *wtr, ClassId classId, ObjectId objectId, T *obj,
                   const PropertyAccessBase *pa, StoreMode mode=StoreMode::force_none, unsigned flags=0) {
    return false;
  }
  template <typename T>
  static bool load(ReadTransaction *tr, ReadBuf &buf, ClassId classId, ObjectId objectId, T *obj,
                   const PropertyAccessBase *pa, StoreMode mode=StoreMode::force_none, unsigned flags=0) {
    return false;
  }
  template <typename T, typename TV>
  static void put(T &d, const PropertyAccessBase *pa, TV &value, unsigned flags) {
  }
  template <typename T, typename TV>
  static void get(T &d, const PropertyAccessBase *pa, TV &value, unsigned flags) {
  }
};

template<typename T>
static PropertyType object_vector_t() {
  using Traits = ClassTraits<T>;
  return PropertyType(Traits::traits_classname, true);
}

template<typename T>
static PropertyType object_t() {
  using Traits = ClassTraits<T>;
  return PropertyType(Traits::traits_classname);
}

} //kv
} //persistence
} //flexis

using NO_SUPERCLASS = flexis::persistence::kv::EmptyClass;

#endif //FLEXIS_FLEXIS_KVTRAITS_H
