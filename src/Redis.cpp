// -*- indent-tabs-mode: nil; tab-width: 4; c-indent-level: 4; c-basic-offset: 4; -*-
//
//  RcppRedis -- Rcpp bindings to Hiredis for some Redis functionality
//
//  Copyright (C) 2013 - 2014    Dirk Eddelbuettel 
//  Portions Copyright (C) 2013  Wush Wu
//  Portions Copyright (C) 2013  William Pleasant
//
//  This file is part of RcppRedis
//
//  RcppRedis is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 2 of the License, or
//  (at your option) any later version.
//
//  RcppRedis is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with RcppRedis.  If not, see <http://www.gnu.org/licenses/>.


// simple C++ class to host a stateful connection to redis
//
// uses hiredis library which provides a basic C API to redis
//
// initially forked from Wush Wu's Rhiredis
// slowly adding some more Redis functions
// trying to figure out if we want
//    -- R serialization for interop with rredis
//    -- 'native' serialization for interop with redis-cli
// or possibly both
//  
// We do use 'binary' serialization for faster processing
//
// Dirk Eddelbuettel, 2013 - 2014


#include <Rcpp.h>
#include <hiredis/hiredis.h>        // we check in configure for this

#include <RApiSerializeAPI.h>   	// provides C API with serialization for R


// A simple and lightweight class -- with just a simple private member variable 
// We could add some more member variables to cache the last call, status, ...

class Redis {

private: 
   
    redisContext *prc_;                // private pointer to redis context

    // set up a connection to Redis on the given machine and port
    void init(std::string host="127.0.0.1", int port=6379)  { 
        prc_ = redisConnect(host.c_str(), port);
        if (prc_->err) 
            Rcpp::stop(std::string("Redis connection error: ") + std::string(prc_->errstr));
    }


    // This function was originally contributed by William Pleasant
    // It switches according to the return type
    SEXP extract_reply(redisReply *reply){
        switch(reply->type) {
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS: 
        case REDIS_REPLY_ERROR: {
            return Rcpp::wrap(std::string(reply->str));
        }
        case REDIS_REPLY_INTEGER: {
            // cast to double to avoid INT_MAX overflow
            return Rcpp::wrap(static_cast<double>(reply->integer));
        }
        case REDIS_REPLY_NIL: {
            return R_NilValue;
        }
        case REDIS_REPLY_ARRAY: {
            return extract_array(reply);
        }
        default:
            Rcpp::stop("Unknown type");
        }
        return R_NilValue;      // makes 'g++ -Wall -pedantic' happy
    }
    
    // for vector results (of any type), fill a list (as a list type can contain
    // different basuc types and we switch later over these
    Rcpp::List extract_array(redisReply *node) {
        Rcpp::List retlist(node->elements);
        for(unsigned int i = 0;i < node->elements;i++) {
            retlist[i] = extract_reply(node->element[i]);
        }
        return retlist;
    }

    // helper function
    std::string replyTypeToString(const redisReply *reply) {
        switch(reply->type) {
        case REDIS_REPLY_STRING:  return std::string("string");
        case REDIS_REPLY_STATUS:  return std::string("status");
        case REDIS_REPLY_INTEGER: return std::string("integer");
        case REDIS_REPLY_ERROR:   return std::string("error");
        case REDIS_REPLY_NIL:     return std::string("nil");
        case REDIS_REPLY_ARRAY:   return std::string("array");
        default:                  return std::string("unknown");
        }
    }

    enum { replyString_t = 0, replyStatus_t, replyInteger_t, replyError_t, 
           replyNil_t, replyArray_t };

    int replyTypeToInteger(const redisReply *reply) {
        switch(reply->type) {
        case REDIS_REPLY_STRING:  return replyString_t;
        case REDIS_REPLY_STATUS:  return replyStatus_t;
        case REDIS_REPLY_INTEGER: return replyInteger_t;
        case REDIS_REPLY_ERROR:   return replyError_t;
        case REDIS_REPLY_NIL:     return replyNil_t;
        case REDIS_REPLY_ARRAY:   return replyArray_t;
        default:                  return -1;
        }
    }

    void checkReplyType(const redisReply *reply, int replyType) {
        if (replyType != replyTypeToInteger(reply)) {
            Rcpp::stop(std::string("Wrong reply type, got ") + replyTypeToString(reply));
        }
    }

public:
   
    Redis(std::string host, int port)  { init(host, port); }
    Redis(std::string host)            { init(host);       }
    Redis()                            { init();           }

    ~Redis() { 
        redisFree(prc_);
        prc_ = NULL;                // just to be on the safe side
    }

    // execute given string
    SEXP exec(std::string cmd) {
        redisReply *reply = static_cast<redisReply*>(redisCommand(prc_, cmd.c_str()));
        SEXP rep = extract_reply(reply);
        freeReplyObject(reply);
        return(rep);
    }


    // redis set -- serializes to R internal format
    std::string set(std::string key, SEXP s) {

        // if raw, use as is else serialize to raw
        Rcpp::RawVector x = (TYPEOF(s) == RAWSXP) ? s : serializeToRaw(s);

        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "SET %s %b", 
                                                  key.c_str(), x.begin(), x.size()));
        std::string res(reply->str);                                                
        freeReplyObject(reply);
        return(res);
    }

    // redis get -- deserializes from R format
    SEXP get(std::string key) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "GET %s", key.c_str()));

        int nc = reply->len;
        Rcpp::RawVector res(nc);
        memcpy(res.begin(), reply->str, nc);
        freeReplyObject(reply);
        SEXP obj = unserializeFromRaw(res);
        return(obj);
    }

    // redis keys -- returns character vector
    SEXP keys(std::string regexp) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "KEYS %s", regexp.c_str()));

        unsigned int nc = reply->elements;
        Rcpp::CharacterVector vec(nc);
        for (unsigned int i = 0; i < nc; i++) {
            vec[i] = reply->element[i]->str;
        }
        freeReplyObject(reply);
        return(vec);
    }

    // redis lrange: get list from start to end -- with R serialization
    Rcpp::List lrange(std::string key, int start, int end) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "LRANGE %s %d %d", 
                                                  key.c_str(), start, end));

        unsigned int len = reply->elements;
        Rcpp::Rcout << "Seeing " << len << " elements\n";
        Rcpp::List x(len);
        for (unsigned int i = 0; i < len; i++) {
            Rcpp::Rcout << "  Seeing size " << reply->element[i]->len << "\n";
            int nc = reply->element[i]->len;
            Rcpp::RawVector res(nc);
            memcpy(res.begin(), reply->element[i]->str, nc);
            SEXP obj = unserializeFromRaw(res);
            x[i] = obj;
        }
                                               
        freeReplyObject(reply);
        return(x);
    }


    // could create new functions to (re-)connect with given host and port etc pp

    
    // Some "non-R-serialzation" functions below

    // used in functions below
    static const unsigned int szdb = sizeof(double);

    // redis set a string without deserializes from R format [as set() above does]
    std::string setString(std::string key, std::string value) {
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "SET %s %s", 
                                                  key.c_str(), value.c_str()));
        std::string res(reply->str);
        freeReplyObject(reply);
        return(res);
    }

    // redis get a string without deserializes from R format [as get() above does]
    std::string getString(std::string key) {
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "GET %s", key.c_str()));
        std::string res(reply->str);
        freeReplyObject(reply);
        return(res);
    }


    // redis "set a vector" -- without R serialization, without attributes, ...
    std::string setVector(std::string key, Rcpp::NumericVector x) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "SET %s %b", 
                                                  key.c_str(), x.begin(), x.size()*szdb));
        std::string res(reply->str);                                                
        freeReplyObject(reply);
        return(res);
    }

    // redis "get a vector" -- without R serialization, without attributes, ...
    Rcpp::NumericVector getVector(std::string key) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "GET %s", key.c_str()));

        int nc = reply->len;
        Rcpp::NumericVector x(nc/szdb);
        memcpy(x.begin(), reply->str, nc);
                                               
        freeReplyObject(reply);
        return(x);
    }


    // redis "get from list from start to end" -- without R serialization
    // assume numerical vectors, doesn't test all that well
    Rcpp::List listRange(std::string key, int start, int end) {

        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "LRANGE %s %d %d", 
                                                  key.c_str(), start, end));

        //Rcpp::Rcout << "listRange got type: " << replyTypeToString(reply) << "\n";
        checkReplyType(reply, replyArray_t); // ensure we got array
        unsigned int len = reply->elements;
        Rcpp::List x(len);
        for (unsigned int i = 0; i < len; i++) {
            checkReplyType(reply->element[i], replyString_t); // ensure we got [binary] string 
            int nc = reply->element[i]->len;
            Rcpp::NumericVector v(nc/szdb);
            memcpy(v.begin(), reply->element[i]->str, nc);
            x[i] = v;
        }
        freeReplyObject(reply);
        return(x);
    }

    // redis "get from list from start to end" as matrix -- without R serialization
    Rcpp::NumericMatrix listToMatrix(Rcpp::List Z) {
        unsigned int n = Z.size();
        int k = Rcpp::NumericVector(Z[0]).size();
        Rcpp::NumericMatrix X(n, k);
        for (unsigned int i = 0; i < n; i++) {
            Rcpp::NumericVector z(Z[i]);
            if (z.size() != k) Rcpp::stop("Wrong dimension");
            X.row(i) = z;
        }
        return(X);
    }

    // redis "lpop from" -- without R serialization
    std::string listLPop(std::string key) {
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "LPOP %s", key.c_str()));

        std::string res;
        if (replyTypeToInteger(reply) == replyNil_t) {
            res = "(nil)";
        } else {
            checkReplyType(reply, replyString_t); // ensure we got string
            res = reply->str;
        }
        freeReplyObject(reply);
        return(res);
    }

    // redis "prepend to list" -- without R serialization
    // as above: pure vector, no attributes, ...
    std::string listLPush(std::string key, Rcpp::NumericVector x) {

        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "LPUSH %s %b", 
                                                  key.c_str(), x.begin(), x.size()*szdb));

        //std::string res(reply->str);                                                
        std::string res = "";
        freeReplyObject(reply);
        return(res);
    }

    // redis "append to list" -- without R serialization
    // as above: pure vector, no attributes, ...
    std::string listRPush(std::string key, Rcpp::NumericVector x) {

        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "RPUSH %s %b", 
                                                  key.c_str(), x.begin(), x.size()*szdb));

        //std::string res(reply->str);                                                
        std::string res = "";
        freeReplyObject(reply);
        return(res);
    }


    // redis "zadd" -- insert score and matrix row (without R serialization)
    // by convention, the first element of each row vector is the score value
    double zadd(std::string key, Rcpp::NumericMatrix x) {

        double res = 0;
        for (int i=0; i<x.nrow(); i++) {
            Rcpp::NumericVector y = x.row(i);
            // uses binary protocol, see hiredis doc at github
            redisReply *reply = 
                static_cast<redisReply*>(redisCommand(prc_, "ZADD %s %f %b", 
                                                      key.c_str(), 
                                                      y[0], 
                                                      y.begin(), y.size()*szdb));
            checkReplyType(reply, replyInteger_t); // ensure we got array
            res += static_cast<double>(reply->integer); // a 'long long' would overflow int
            freeReplyObject(reply);
        }
        return(res);
    }


    // redis "zrange" (withscores) -- retrieve vectors from index [min, max] (without R serial.)
    Rcpp::NumericMatrix zrange(std::string key, int min, int max) {
        
        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "ZRANGE %s %d %d", 
                                                  key.c_str(), min, max));
        checkReplyType(reply, replyArray_t); // ensure we got array
        unsigned int rows = reply->elements;
        unsigned int cols = reply->element[0]->len; // assumes all row have same nb of cols
        Rcpp::NumericMatrix x(rows, cols/szdb);     // and that all elements are type double
        for (unsigned int i = 0; i < rows; i++) {
            checkReplyType(reply->element[i], replyString_t); // ensure we got [binary] string 
            Rcpp::NumericVector v(cols/szdb);
            memcpy(v.begin(), reply->element[i]->str, cols);
            x.row(i) = v;
        }
        freeReplyObject(reply);
        return(x);
    }

    // redis "zrangebyscore" -- retrieve vectors for score in [min, max] (without R serial.)
    Rcpp::NumericMatrix zrangebyscore(std::string key, double min, double max) {
        
        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, 
                                                  "ZRANGEBYSCORE %s %f %f", 
                                                  key.c_str(), min, max));
        checkReplyType(reply, replyArray_t); // ensure we got array
        unsigned int rows = reply->elements;
        unsigned int cols = reply->element[0]->len; // assumes all row have same nb of cols
        Rcpp::NumericMatrix x(rows, cols/szdb);     // and that all elements are type double
        for (unsigned int i = 0; i < rows; i++) {
            checkReplyType(reply->element[i], replyString_t); // ensure we got [binary] string 
            Rcpp::NumericVector v(cols/szdb);
            memcpy(v.begin(), reply->element[i]->str, cols);
            x.row(i) = v;
        }
        freeReplyObject(reply);
        return(x);
    }

    // redis "zremrangebyscore" -- remove elements in [min, max] 
    double zremrangebyscore(std::string key, double min, double max) {
        
        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, 
                                                  "ZREMRANGEBYSCORE %s %f %f", 
                                                  key.c_str(), min, max));
        checkReplyType(reply, replyInteger_t); // ensure we got array
        double res = static_cast<double>(reply->integer); // a 'long long' would overflow int
        freeReplyObject(reply);
        return(res);
    }

    // redis "zcard" -- number of members in sorted set
    double zcard(std::string key) {
        
        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, 
                                                  "ZCARD %s", key.c_str()));
        checkReplyType(reply, replyInteger_t); // ensure we got integer
        double res = static_cast<double>(reply->integer); // a 'long long' would overflow int
        freeReplyObject(reply);
        return(res);
    }

    // redis "zcount" -- counter members in set with scores in given range
    Rcpp::NumericMatrix zcount(std::string key, double min, double max) {
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, 
                                                  "ZCOUNT %s %f %f", 
                                                  key.c_str(), min, max));
        checkReplyType(reply, replyInteger_t); // ensure we got array
        double res = static_cast<double>(reply->integer); // a 'long long' would overflow int
        freeReplyObject(reply);
        return(res);
    }

    // redis "get from list from start to end" -- without R serialization
    // assume strings numerical vectors, doesn't test all that well
    Rcpp::CharacterVector listRangeAsStrings(std::string key, int start, int end) {

        // uses binary protocol, see hiredis doc at github
        redisReply *reply = 
            static_cast<redisReply*>(redisCommand(prc_, "LRANGE %s %d %d", 
                                                  key.c_str(), start, end));

        //Rcpp::Rcout << "listRange got type: " << replyTypeToString(reply) << "\n";
        checkReplyType(reply, replyArray_t); // ensure we got array
        unsigned int len = reply->elements;
        Rcpp::CharacterVector x(len);
        for (unsigned int i = 0; i < len; i++) {
            checkReplyType(reply->element[i], replyString_t); // ensure we got [binary] string 
            x[i] = reply->element[i]->str;
        }
        freeReplyObject(reply);
        return(x);
    }


};


RCPP_MODULE(Redis) {
    Rcpp::class_<Redis>("Redis")   
        
        .constructor("default constructor")  
        .constructor<std::string>("constructor with host port")  
        .constructor<std::string, int>("constructor with host and port")  

        .method("exec", &Redis::exec,  "execute given redis command and arguments")

        .method("set",  &Redis::set,   "runs 'SET key object', serializes internally")
        .method("get",  &Redis::get,   "runs 'GET key', deserializes internally")

        .method("keys", &Redis::keys,  "runs 'KEYS expr', returns character vector")

        .method("lrange",  &Redis::lrange,   "runs 'LRANGE key start end' for list")

        // non-R serialization methods below
        .method("setString",  &Redis::setString,   "runs 'SET key obj' without serialization")
        .method("getString",  &Redis::getString,   "runs 'GET key' without deserialization")

        .method("setVector",  &Redis::setVector,   "runs 'SET key object' for a numeric vector")
        .method("getVector",  &Redis::getVector,   "runs 'GET key object' for a numeric vector")
        .method("listLPop",   &Redis::listLPop,    "pops numeric vector to list")
        .method("listLPush",  &Redis::listLPush,   "prepends numeric vector to list")
        .method("listRPush",  &Redis::listRPush,   "appends numeric vector to list")
        .method("listRange",  &Redis::listRange,   "runs 'LRANGE key start end' for list, native")
        .method("zadd",       &Redis::zadd,        "inserts rows of matrix into sorted set, first value is score, binary")

        .method("zrange",     &Redis::zrange,      "retrieve sorted range over index [min, max], binary")
        .method("zrangebyscore", &Redis::zrangebyscore,  "retrieve sorted range over score [min, max], binary")
        .method("zremrangebyscore", &Redis::zremrangebyscore,  "remove sorted range in [min, max]")
        .method("zcard",      &Redis::zcard,       "get number of members in sorted set")
        .method("zcount",     &Redis::zcount,      "get number of members in sorted set within range [min,max]")

        .method("listToMatrix",  &Redis::listToMatrix,  "convert list of vectors into matrix")

        .method("listRangeAsStrings",  &Redis::listRangeAsStrings,   "runs 'LRANGE key start end' for list, returns string vector")

    ;
}