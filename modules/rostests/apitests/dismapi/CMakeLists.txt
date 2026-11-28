add_executable(dismapi_apitest ProvisionedAppx.c testlist.c)
set_module_type(dismapi_apitest win32cui)
add_importlibs(dismapi_apitest msvcrt kernel32)
add_rostests_file(TARGET dismapi_apitest)
