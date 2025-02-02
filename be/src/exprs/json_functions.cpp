// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exprs/json_functions.h"

#include <stdlib.h>
#include <sys/time.h>

#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <re2/re2.h>

#include "exprs/expr.h"
#include "exprs/anyval_util.h"
#include "common/logging.h"
#include "olap/olap_define.h"
#include "runtime/string_value.h"
#include "runtime/tuple_row.h"
#include "rapidjson/error/en.h"

namespace doris {

// static const re2::RE2 JSON_PATTERN("^([a-zA-Z0-9_\\-\\:\\s#\\|\\.]*)(?:\\[([0-9]+)\\])?");
// json path cannot contains: ", [, ]
static const re2::RE2 JSON_PATTERN("^([^\\\"\\[\\]]*)(?:\\[([0-9]+)\\])?");

void JsonFunctions::init() {
}

IntVal JsonFunctions::get_json_int(
        FunctionContext* context, const StringVal& json_str, const StringVal& path) {
    if (json_str.is_null || path.is_null) {
        return IntVal::null();
    }
    std::string json_string((char*)json_str.ptr, json_str.len);
    std::string path_string((char*)path.ptr, path.len);
    rapidjson::Document document;
    rapidjson::Value* root =
        get_json_object(context, json_string, path_string, JSON_FUN_INT, &document);
    if (root->IsInt()) {
        return IntVal(root->GetInt());
    } else {
        return IntVal::null();
    }
}

StringVal JsonFunctions::get_json_string(
        FunctionContext* context, const StringVal& json_str, const StringVal& path) {
    if (json_str.is_null || path.is_null) {
        return StringVal::null();
    }

    std::string json_string((char*)json_str.ptr, json_str.len);
    std::string path_string((char*)path.ptr, path.len);
    rapidjson::Document document;
    rapidjson::Value* root =
        get_json_object(context, json_string, path_string, JSON_FUN_STRING, &document);
    if (root->IsNull()) {
        return StringVal::null();
    } else if (root->IsString()) {
        return AnyValUtil::from_string_temp(context, root->GetString());
    } else {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
        root->Accept(writer);
        return AnyValUtil::from_string_temp(context, std::string(buf.GetString()));
    }
}

DoubleVal JsonFunctions::get_json_double(
        FunctionContext* context, const StringVal& json_str, const StringVal& path) {
    if (json_str.is_null || path.is_null) {
        return DoubleVal::null();
    }
    std::string json_string((char*)json_str.ptr, json_str.len);
    std::string path_string((char*)path.ptr, path.len);
    rapidjson::Document document;
    rapidjson::Value* root =
        get_json_object(context, json_string, path_string, JSON_FUN_DOUBLE, &document);
    if (root->IsInt()) {
        return DoubleVal(static_cast<double>(root->GetInt()));
    } else if (root->IsDouble()) {
        return DoubleVal(root->GetDouble());
    } else {
        return DoubleVal::null();
    }
}

rapidjson::Value* JsonFunctions::get_json_object(
        FunctionContext* context,
        const std::string& json_string,
        const std::string& path_string,
        const JsonFunctionType& fntype,
        rapidjson::Document* document) {

    // split path by ".", and escape quota by "\"
    // eg:
    //    '$.text#abc.xyz'  ->  [$, text#abc, xyz]
    //    '$."text.abc".xyz'  ->  [$, text.abc, xyz]
    //    '$."text.abc"[1].xyz'  ->  [$, text.abc[1], xyz]
    std::vector<JsonPath>* parsed_paths;
    std::vector<JsonPath> tmp_parsed_paths;
#ifndef BE_TEST
    parsed_paths = reinterpret_cast<std::vector<JsonPath>*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
    if (parsed_paths == nullptr) {
        boost::tokenizer<boost::escaped_list_separator<char> > tok(path_string, boost::escaped_list_separator<char>("\\", ".", "\""));
        std::vector<std::string> paths(tok.begin(), tok.end());
        get_parsed_paths(paths, &tmp_parsed_paths);
        parsed_paths = &tmp_parsed_paths;
    }
#else
    boost::tokenizer<boost::escaped_list_separator<char> > tok(path_string, boost::escaped_list_separator<char>("\\", ".", "\""));
    std::vector<std::string> paths(tok.begin(), tok.end());
    get_parsed_paths(paths, &tmp_parsed_paths);
    parsed_paths = &tmp_parsed_paths;
#endif

    VLOG(10) << "first parsed path: " << (*parsed_paths)[0].debug_string();

    if (!(*parsed_paths)[0].is_valid) {
        return document;
    }

    if (UNLIKELY((*parsed_paths).size() == 1)) {
        if (fntype == JSON_FUN_STRING) {
            document->SetString(json_string.c_str(), document->GetAllocator());
        } else {
            return document;
        }
    }

    //rapidjson::Document document;
    document->Parse(json_string.c_str());
    if (UNLIKELY(document->HasParseError())) {
        LOG(ERROR) << "Error at offset " << document->GetErrorOffset()
            << ": " << GetParseError_En(document->GetParseError());
        document->SetNull();
        return document;
    }

    rapidjson::Value* root = document;
    rapidjson::Value* array_obj = NULL;
    for (int i = 1; i < (*parsed_paths).size(); i++) {
        VLOG(10) << "parsed_paths: " << (*parsed_paths)[i].debug_string();

        if (root->IsNull()) {
            break;
        }

        if (UNLIKELY(!(*parsed_paths)[i].is_valid)) {
            root->SetNull();
        }

        std::string& col = (*parsed_paths)[i].key;
        int index = (*parsed_paths)[i].idx;
        if (LIKELY(!col.empty())) {
            if (root->IsArray()) {
                array_obj = static_cast<rapidjson::Value*>(
                        document->GetAllocator().Malloc(sizeof(rapidjson::Value)));
                array_obj->SetArray();
                bool is_null = true;

                // if array ,loop the array,find out all Objects,then find the results from the objects
                for (int j = 0; j < root->Size(); j++) {
                    rapidjson::Value* json_elem = &((*root)[j]);

                    if (json_elem->IsArray() || json_elem->IsNull()) {
                        continue;
                    } else {
                        if (!json_elem->IsObject() || !json_elem->HasMember(col.c_str())) {
                            continue;
                        }
                        rapidjson::Value* obj = &((*json_elem)[col.c_str()]);

                        if (obj->IsArray()) {
                            is_null = false;
                            for (int k = 0; k < obj->Size(); k++) {
                                array_obj->PushBack((*obj)[k], document->GetAllocator());
                            }
                        } else if (!obj->IsNull()) {
                            is_null = false;
                            array_obj->PushBack(*obj, document->GetAllocator());
                        }
                    }
                }

                root = is_null ? &(array_obj->SetNull()) : array_obj;
            } else if (root->IsObject()){
                if (!root->HasMember(col.c_str())) {
                    root->SetNull();
                } else {
                    root = &((*root)[col.c_str()]);
                }
            } else {
                // root is not a nested type, return NULL
                root->SetNull();
            }
        }

        if (UNLIKELY(index != -1)) {
            // judge the rapidjson:Value, which base the top's result,
            // if not array return NULL;else get the index value from the array
            if (root->IsArray()) {
                if (root->IsNull() || index >= root->Size()) {
                    root->SetNull();
                } else {
                    root = &((*root)[index]);
                }
            } else {
                root->SetNull();
            }
        }
     }

     return root;
}

void JsonFunctions::json_path_prepare(
        doris_udf::FunctionContext* context,
        doris_udf::FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return;
    }

    if (!context->is_arg_constant(1)) {
        return;
    }
    StringVal* path = reinterpret_cast<StringVal*>(context->get_constant_arg(1));
    if (path->is_null) {
        return;
    }

    boost::tokenizer<boost::escaped_list_separator<char> > tok(
            std::string(reinterpret_cast<char*>(path->ptr), path->len),
            boost::escaped_list_separator<char>("\\", ".", "\""));
    std::vector<std::string> path_exprs(tok.begin(), tok.end());
    std::vector<JsonPath>* parsed_paths = new std::vector<JsonPath>();
    get_parsed_paths(path_exprs, parsed_paths);

    context->set_function_state(scope, parsed_paths);
    VLOG(10) << "prepare json path. size: " << parsed_paths->size();
}

void JsonFunctions::get_parsed_paths(
        const std::vector<std::string>& path_exprs,
        std::vector<JsonPath>* parsed_paths) {

    if (path_exprs[0] != "$") {
        parsed_paths->emplace_back("", -1, false);
    } else {
        parsed_paths->emplace_back("$", -1, true);
    }

    for (int i = 1; i < path_exprs.size(); i++) {
        std::string col;
        std::string index;
        if (UNLIKELY(!RE2::FullMatch(path_exprs[i], JSON_PATTERN, &col, &index))) {
            parsed_paths->emplace_back("", -1, false);
        } else {
            int idx = -1;
            if (!index.empty()) {
                idx = atoi(index.c_str());
            }
            parsed_paths->emplace_back(col, idx, true);
        }
    }
}

void JsonFunctions::json_path_close(
        doris_udf::FunctionContext* context,
        doris_udf::FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return;
    }
    std::vector<JsonPath>* parsed_paths = reinterpret_cast<std::vector<JsonPath>*>(context->get_function_state(scope));
    if (parsed_paths != nullptr) {
        delete parsed_paths;
        VLOG(10) << "close json path";
    }
}

}

