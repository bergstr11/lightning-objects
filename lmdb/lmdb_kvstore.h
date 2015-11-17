//
// Created by chris on 10/7/15.
//

#ifndef FLEXIS_LMDBSTORE_H
#define FLEXIS_LMDBSTORE_H

#include "../kvstore.h"

namespace flexis {
namespace persistence {
namespace lmdb {

class FlexisPersistence_EXPORT KeyValueStore : public flexis::persistence::KeyValueStore
{
public:
  struct Options {
    const unsigned initialMapSizeMB = 1;
    const unsigned minTransactionSpaceKB = 512;
    const unsigned increaseMapSizeKB = 512;
    const bool lockFile = false;
    const bool writeMap = true;

    Options(unsigned mapSizeMB = 1024, bool lockFile = false, bool writeMap = false)
        : initialMapSizeMB(mapSizeMB), lockFile(lockFile), writeMap(writeMap) {}
  };

  struct FlexisPersistence_EXPORT Factory
  {
    const std::string location, name;
    const Options options;

    Factory(std::string location, std::string name, Options options = Options())
        : location(location), name(name), options(options) {}
    operator flexis::persistence::KeyValueStore *() const;
  };
};

} //lmdb
} //persistence
} //flexis


#endif //FLEXIS_LMDBSTORE_H
