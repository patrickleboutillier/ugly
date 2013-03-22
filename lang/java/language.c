#include <stdlib.h>
#include "ugly_java_priv.h"


ugly_interface *_ugly_java_language_init(ugly_context *ctx, const char *name, const char *dir){
	ugly_interface *iface = UGLY_INIT_IFACE(iface, java) ;

    return iface ;
}
