#! /bin/bash

#-------------------------------------------------------------------------
#
# autogen.sh
#             Foreign-data wrapper for remote MongoDB servers
#
# Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
# Portions Copyright (c) 2004-2024, EnterpriseDB Corporation.
#
# IDENTIFICATION
#             autogen.sh
#
#-------------------------------------------------------------------------


MONGOC_VERSION=1.17.3
JSONC_VERSION=0.15-20200726
MONGOC_INSTALL="${MONGOC_INSTALL_DIR}"
JSONC_INSTALL="${JSONC_INSTALL_DIR}"

# Don't allow input to the script
if [ "$#" -ne 0 ]; then
    echo "Usage: autogen.sh"
    exit
fi

CMAKE_COMMAND='cmake3'
if ! [ -x "$(command -v cmake3)" ]; then
    CMAKE_COMMAND='cmake'
fi

###
# Pull the latest version of Mongo C Driver's master branch
#
function checkout_mongo_driver
{
	rm -rf mongo-c-driver &&
	wget https://github.com/mongodb/mongo-c-driver/releases/download/$MONGOC_VERSION/mongo-c-driver-$MONGOC_VERSION.tar.gz &&
	tar -zxf mongo-c-driver-$MONGOC_VERSION.tar.gz &&
	mv mongo-c-driver-$MONGOC_VERSION mongo-c-driver &&
	rm -rf mongo-c-driver-$MONGOC_VERSION.tar.gz
}

##
# Pull the json-c library
#
function checkout_json_lib
{
	echo $PWD &&
	rm -rf json-c &&
	wget https://github.com/json-c/json-c/archive/json-c-$JSONC_VERSION.tar.gz &&
	tar -zxf json-c-$JSONC_VERSION.tar.gz &&
	mv json-c-json-c-$JSONC_VERSION json-c &&
	rm -rf json-c-$JSONC_VERSION.tar.gz &&
	echo $PWD
}


##
# Compile and install json-c library
#
function install_json_lib
{
	cd json-c &&
	$CMAKE_COMMAND -DCMAKE_INSTALL_PREFIX=$JSONC_INSTALL $JSONC_CFLAGS . &&
	make install &&
	cd ..
}

###
# Configure and install the Mongo C Driver and libbson
#
function install_mongoc_driver
{
	cd mongo-c-driver &&
	$CMAKE_COMMAND -DCMAKE_INSTALL_PREFIX=$MONGOC_INSTALL -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DENABLE_SSL=AUTO . &&
	make install &&
	cd ..
}

checkout_mongo_driver &&
checkout_json_lib &&
install_mongoc_driver &&
install_json_lib &&
export PKG_CONFIG_PATH=mongo-c-driver/src/libmongoc/src:mongo-c-driver/src/libbson/src

ret=$?
if [ "$ret" -ne 0 ]; then
	echo "Failed"
	exit $ret
else
	echo "Done"
	exit 0
fi
