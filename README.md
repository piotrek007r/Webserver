# Webserv

HTTP/1.1 server written in C++98. 42 Warsaw project, we did it in a team of three:

- Piotr Ruszkiewicz (me)
- Antoni Górski ([antekgorski](https://github.com/antekgorski))
- Grzegorz Buczyński ([GrzegorzBuczynski](https://github.com/GrzegorzBuczynski))

This repo is a copy of our team repo with the whole history, so you can see who did what in the commits.

## What it can do

- one `poll()` loop for all clients, nothing blocks
- config file similar to nginx: ports, server blocks, locations, error pages, body size limit, redirects
- GET, POST (file upload) and DELETE
- CGI (python, php, bash)
- directory listing

## My part

I wrote the core engine (`coreEngine/`): sockets, the `poll()` loop and talking with clients.
`recv()` almost never gives you the whole request at once, so every client has its own buffer
and the request goes further only when it is complete. Same with `send()`, it can send only part
of the response, so the rest waits for the next round. I also did client timeouts and signal handling.

Later I found a bug in the loop: when a client got closed inside the loop, the code still read
`pollFDs[i]` which was already removed. Old gcc didn't complain, GCC 15 stopped the server
right after the first request. Fixed in commit 98c8e8f.

## Build and run

```
make
./webserv conf/default.conf
```

Then open http://localhost:9090

More about modules: [config reader](configReader/README.md), [core engine](coreEngine/READEME.md), [CGI](cgi/READEME.md), [errors](errors/README.md)
