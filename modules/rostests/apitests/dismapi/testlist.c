#define STANDALONE
#include <apitest.h>

extern void func_ProvisionedAppx(void);

const struct test winetest_testlist[] =
{
    { "ProvisionedAppx", func_ProvisionedAppx },
    { 0, 0 }
};
