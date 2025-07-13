#Concurrent HTTP Server

Handles Several GET and POST requests concurrently with the aid of
a threadsafe queue and reader/writer lock implementation. 

In addition to these modules, a few obfuscated helpers provide convienient interfaces 
to the listener socket, HTTP protocol macros, request, and response semantics.

To Build:

Run cd concurrentHTTPServer

make 

A set of local tests (@author Andrew Quinn) may be used individually by running 

#./<name of script>.sh within the directory after building

or simply run ./test_repo.sh run all tests at once
