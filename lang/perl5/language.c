#include <stdlib.h>
#include "ugly_perl5_priv.h"


ugly_interface *_ugly_perl5_language_init(ugly_context *ctx, const char *name, const char *dir){
	ugly_interface *iface = UGLY_INIT_IFACE(iface, perl5) ;

    return iface ;
}
