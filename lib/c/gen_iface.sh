#!/bin/bash

echo "/* This file was generated automatically on $(/bin/date) */"
echo
echo "struct _ugly_interface {"
cat ugly_api.h | while read tag rtype func args ; do
	if [ "$tag" = "UGLY_IFACE_V" ] ; then
		rtype='void*'
		tag=UGLY_IFACE
	fi
	if [ "$tag" = "UGLY_IFACE" ] ; then
		name=${func#ugly_}
		echo "    $rtype (*$name)$args"
	fi
done
echo "} ;"
echo 


echo "#define UGLY_INIT_IFACE(iface, lang) \\"
echo "    (ugly_interface *)malloc(sizeof(ugly_interface)) ; \\"
cat ugly_api.h | while read tag rtype func args ; do
	if [ "$tag" = "UGLY_IFACE_V" ] ; then
		rtype='void*'
		tag=UGLY_IFACE
	fi
	if [ "$tag" = "UGLY_IFACE" ] ; then
		name=${func#ugly_}
		echo "    $rtype _ugly_ ## lang ## _$name$args \\"
		echo "    iface->$name = _ugly_ ## lang ## _$name ; \\"
	fi
done
echo 

