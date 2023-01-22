/*
    Simple portable memory io for lua/luajit.
    muragami, muragami@wishray.com, Jason A. Petrasko 2023
    MIT License: https://opensource.org/licenses/MIT    
*/

#include "lauxlib.h"

// push an IO interface table for a block of memory
int lua_pushIO(lua_State *L, void *mem, unsigned long bytes, unsigned int local);
