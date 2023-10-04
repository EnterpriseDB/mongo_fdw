Notes about installation Mongo Foreign Data Wrapper
===================================================

To compile the [MongoDB][1] foreign data wrapper for [PostgreSQL](https://www.postgresql.org/), `mongo-c` and `json-c`
libraries are needed. To build and install `mongo-c` and `json-c` libraries, there
are two ways. You can either use script `autogen.sh` or you can manually
perform all required steps listed.

### Notes about new MongoDB C Driver support
This enhancement is to add a new [MongoDB][1]' C driver. The current
implementation is based on the legacy driver of MongoDB. But
[MongoDB][1] is provided completely new library for driver called
MongoDB's meta driver. Added support for the same. Now compile time
option is available to use legacy and meta driver.

In order to use MongoDB driver 1.17.0+, take the following steps:

  * clone `libmongoc` version 1.17.0+
    (https://github.com/mongodb/mongo-c-driver) and follow the install
    directions given there.  `libbson` is now maintained in a subdirectory
    of the `libmongoc`.
    (https://github.com/mongodb/mongo-c-driver/tree/master/src/libbson).
  * ensure pkg-config / pkgconf is installed on your system.
  * run `make -f Makefile.meta && make -f Makefile.meta install`
  * if you get an error when trying to `CREATE EXTENSION `mongo_fdw`;`,
    then try running `ldconfig`

## Installation using script
Number of manual steps needs to be performed to compile and install required
mongo-c and json-c libraries. If you want to avoid the manual steps, there is a
shell script available which will download and install the appropriate drivers
and libraries for you.

Here is how it works:

To install mongo-c and json-c libraries at custom locations, you need to
export environment variables `MONGOC_INSTALL_DIR` and `JSONC_INSTALL_DIR`
respectively. If these variables are not set then these libraries will be
installed in the default location. Please note that you need to have the
required permissions on the directory where you want to install the libraries.

Build with [MongoDB][1]'s legacy branch driver (Deprecated in mongo_fdw 5.4.0)
   * autogen.sh --with-legacy

Build [MongoDB][1]'s master branch driver
   * autogen.sh --with-master

The script autogen.sh will do all the necessary steps to build with legacy and
meta driver accordingly.

## Steps for manual installation
### mongo-c
#### meta driver
1. Download and extract source code of mongoc driver for version `1.17.3`

	```sh
	wget https://github.com/mongodb/mongo-c-driver/releases/download/1.17.3/mongo-c-driver-1.17.3.tar.gz
	tar xzf mongo-c-driver-1.17.3.tar.gz
	rm -rf mongo-c-driver
	mv mongo-c-driver-1.17.3 mongo-c-driver
	cd mongo-c-driver
	```

2. Configure mongoc driver

	```sh
	cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF .
	```

	To install at custom location:

	```sh
	cmake -DCMAKE_INSTALL_PREFIX=YOUR_INSTALLATION_DIRECTORY -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF .
	```

3. Compile and install

	```sh
	cmake --build .
	cmake --build . --target install
	```

For more details on installation of mongo-c driver, you can refer [here][3].

#### Legacy driver

Deprecation Notice:
The legacy driver support has been deprecated in mongo_fdw 5.4.0 and is
expected to be removed entirely in a future release.

* Checkout, extract legacy branch

	```sh
	wget https://github.com/mongodb/mongo-c-driver/archive/v0.8.tar.gz
	tar -zxf v0.8.tar.gz
	rm -rf mongo-c-driver
	mv  mongo-c-driver-0.8 mongo-c-driver
	```

### json-c

1. Download and extract source code

	```sh
	wget https://github.com/json-c/json-c/archive/json-c-0.15-20200726.tar.gz
	tar -xzf json-c-0.15-20200726.tar.gz
	rm -rf json-c
	mv json-c-json-c-0.15-20200726/ json-c
	cd json-c
	```

2. Configure

	```sh
	cmake .
	```
	To install at custom location:

	```sh
	cmake -DCMAKE_INSTALL_PREFIX=YOUR_INSTALLATION_DIRECTORY .
	```

3. Compile and install

	```sh
	make
	make install
	```

For more details on installation of json-c library, you can refer [here][4].

### How to compile against mongo-c Meta or Legacy driver?
To compile against legacy driver, 'Makefile.legacy' must be used and
'Makefile.meta' must be used to compile against the meta driver. For example,
this can be achieved by copying required Makefile as shown below:
For meta,

	cp Makefile.meta Makefile

For legacy (Deprecated in mongo_fdw 5.4.0),

	cp Makefile.legacy Makefile

The default compilation is with Meta driver.

## Mongo_fdw configuration, compilation and installation
The `PKG_CONFIG_PATH` environment variable must be set to mongo-c-driver source
directory for successful compilation as shown below,

```sh
export PKG_CONFIG_PATH=$YOUR_MONGO_FDW_SOURCE_DIR/mongo-c-driver/src/libmongoc/src:$YOUR_MONGO_FDW_SOURCE_DIR/mongo-c-driver/src/libbson/src
```

The `LD_LIBRARY_PATH` environment variable must include the path to the mongo-c
installation directory containing the libmongoc-1.0.so and libbson-1.0.so
files. For example, assuming the installation directory is /home/mongo-c and
the libraries were created under it in lib64 sub-directory, then we can define
the `LD_LIBRARY_PATH` as:

```sh
export LD_LIBRARY_PATH=/home/mongo-c/lib64:$LD_LIBRARY_PATH
```

Note: This `LD_LIBRARY_PATH` environment variable setting must be in effect
when the `pg_ctl` utility is executed to start or restart PostgreSQL or
EDB Postgres Advanced Server.


1. To build on POSIX-compliant systems you need to ensure the
   `pg_config` executable is in your path when you run `make`. This
   executable is typically in your PostgreSQL installation's `bin`
   directory. For example:

    ```sh
    export PATH=/usr/local/pgsql/bin/:$PATH
    ```

2. Compile the code using make.

    ```sh
    make USE_PGXS=1
    ```

3. Finally install the foreign data wrapper.

    ```sh
    make USE_PGXS=1 install
    ```

4. Running regression test.

    ```sh
    make USE_PGXS=1 installcheck
    ```
   However, make sure to set the `MONGO_HOST`, `MONGO_PORT`, `MONGO_USER_NAME`,
   and `MONGO_PWD` environment variables correctly. The default settings can be
   found in the `mongodb_init.sh` script.


If you run into any issues, please [let us know][2].

[1]: http://www.mongodb.com
[2]: https://github.com/enterprisedb/mongo_fdw/issues/new
[3]: http://mongoc.org/libmongoc/1.17.3/installing.html#configuring-the-build
[4]: https://github.com/json-c/json-c/tree/json-c-0.15-20200726#build-instructions--
