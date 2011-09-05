# spotify-api-server
stripped version of liesens spotify-api-server https://github.com/liesen/spotify-api-server

## Supported API methods

### Playlists

    GET /playlist/{id} -> <playlist>

## How to build

1. Make sure you have the required libraries
  * subversion (`libsvn-dev`) and its dependency, `libapr`
  * [libevent](http://monkey.org/~provos/libevent/)
  * [jansson](http://www.digip.org/jansson/) > 2.0

1. Update `account.c` with your credentials. A *Spotify premium account is necessary*.

1. Copy `appkey.c` into the directory and run `make`.

