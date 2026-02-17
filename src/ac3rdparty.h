#ifndef AC3RDPARTY_H
#define AC3RDPARTY_H
#include "config.h"

namespace acng {

/**
 * @brief RAII helper for external library setup
 *
 * Initialize and safely cleanup global library resources in global context.
 * Supposed to be used as RAII helper in main() methods.
 */
class ACNG_API ac3rdparty
{
//    struct tMisc;
//    tMisc *m_misc;
public:
    ac3rdparty();
    ~ac3rdparty();
};

}

#endif // AC3RDPARTY_H
