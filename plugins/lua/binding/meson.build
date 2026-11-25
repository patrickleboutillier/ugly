# The Lua binding: lua.c again, as the Lua C module "ugly" (ugly.so), for
# require "ugly" from a plain lua process. It doesn't link liblua: its Lua
# symbols come from the host interpreter (a second Lua in the process would
# be fatal). Built once per Lua found, in <version>/ (same file there), since
# a host's Lua is fixed: binding_dirs gets 'lua5.4', 'lua5.3', and 'lua' for
# the plugin's version.

foreach v, d : lua_deps
  binding_lua_version = v
  binding_lua_dep = d
  subdir(v)
endforeach
binding_dirs += { 'lua': binding_dirs['lua' + lua_version] }
