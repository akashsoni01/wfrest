#include "workflow/HttpUtil.h"
#include "workflow/MySQLResult.h"
#include "workflow/WFMySQLConnection.h"
#include "workflow/Workflow.h"

#include <unistd.h>
#include <algorithm>

#include "wfrest/HttpMsg.h"
#include "wfrest/UriUtil.h"
#include "wfrest/PathUtil.h"
#include "wfrest/HttpFile.h"
#include "wfrest/json.hpp"
#include "wfrest/HttpServerTask.h"
#include "wfrest/MysqlUtil.h"
#include "wfrest/StatusCode.h"
#include "HttpMsg.h"

using namespace wfrest;
using namespace protocol;
using Json = nlohmann::json;

namespace wfrest
{

struct ReqData
{
    std::string body;
    std::map<std::string, std::string> form_kv;
    Form form;
    Json json;
};

struct ProxyCtx
{
    std::string url;
    HttpServerTask *server_task;
    bool is_keep_alive;
};

void proxy_http_callback(WFHttpTask *http_task)
{   
    int state = http_task->get_state();
    int error = http_task->get_error();
    auto *proxy_ctx = static_cast<ProxyCtx *>(http_task->user_data);
    
    HttpServerTask *server_task = proxy_ctx->server_task;
    HttpResponse *http_resp = http_task->get_resp();
    HttpResp *server_resp = server_task->get_resp();
    // Some servers may close the socket as the end of http response. 
    if (state == WFT_STATE_SYS_ERROR && error == ECONNRESET)
        state = WFT_STATE_SUCCESS;

    if (state == WFT_STATE_SUCCESS)
    {
        server_task->add_callback([proxy_ctx](HttpTask *server_task)
        {
            HttpResp *server_resp = server_task->get_resp();
            size_t size = server_resp->get_output_body_size();
            if (server_task->get_state() != WFT_STATE_SUCCESS)
                fprintf(stderr, "%s: Reply failed: %s, BodyLength: %zu\n",
                        proxy_ctx->url.c_str(), strerror(server_task->get_error()), size);

            delete proxy_ctx;
        });

        const void *body;
        size_t len;
        // Copy the remote webserver's response, to server response.
        if (http_resp->get_parsed_body(&body, &len))
            http_resp->append_output_body_nocopy(body, len);

        HttpResp resp(std::move(*http_resp));
        *server_resp = std::move(resp);
        
        if (!proxy_ctx->is_keep_alive)
            server_resp->set_header_pair("Connection", "close");
    }
    else
    {
        const char *err_string;
        int error = http_task->get_error();

        if (state == WFT_STATE_SYS_ERROR)
            err_string = strerror(error);
        else if (state == WFT_STATE_DNS_ERROR)
            err_string = gai_strerror(error);
        else if (state == WFT_STATE_SSL_ERROR)
            err_string = "SSL error";
        else /* if (state == WFT_STATE_TASK_ERROR) */
            err_string = "URL error (Cannot be a HTTPS proxy)";

        fprintf(stderr, "%s: Fetch failed. state = %d, error = %d: %s\n",
                proxy_ctx->url.c_str(), state, http_task->get_error(),
                err_string);

        server_resp->set_status_code("404");
        server_resp->append_output_body_nocopy("<html>404 Not Found.</html>", 27);
    }
}

Json mysql_concat_json_res(WFMySQLTask *mysql_task)
{
    Json json;
    MySQLResponse *mysql_resp = mysql_task->get_resp();
    MySQLResultCursor cursor(mysql_resp);
    const MySQLField *const *fields;
    std::vector<MySQLCell> arr;

    if (mysql_task->get_state() != WFT_STATE_SUCCESS)
    {
        json["error"] = WFGlobal::get_error_string(mysql_task->get_state(),
                                                    mysql_task->get_error());
        return json;
    }

    do {
        Json result_set;
        if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT &&
            cursor.get_cursor_status() != MYSQL_STATUS_OK)
        {
            break;
        }

        if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT)
        {
            result_set["field_count"] = cursor.get_field_count();
            result_set["rows_count"] = cursor.get_rows_count();
            fields = cursor.fetch_fields();
            std::vector<std::string> fields_name;
            std::vector<std::string> fields_type;
            for (int i = 0; i < cursor.get_field_count(); i++)
            {
                if (i == 0)
                {
                    std::string database = fields[i]->get_db();
                    if(!database.empty())
                        result_set["database"] = std::move(database);
                    result_set["table"] = fields[i]->get_table();
                }
                
                fields_name.push_back(fields[i]->get_name());
                fields_type.push_back(datatype2str(fields[i]->get_data_type()));
            }
            result_set["fields_name"] = fields_name;
            result_set["fields_type"] = fields_type;

            while (cursor.fetch_row(arr))
            {
                Json row;                  
                for (size_t i = 0; i < arr.size(); i++)
                {
                    if (arr[i].is_string())
                    {
                        row.push_back(arr[i].as_string());
                    } 
                    else if (arr[i].is_time() || arr[i].is_datetime()) 
                    {
                        row.push_back(MySQLUtil::to_string(arr[i]));
                    } 
                    else if (arr[i].is_null()) 
                    {
                        row.push_back("NULL");
                    } 
                    else if(arr[i].is_double()) 
                    {
                        row.push_back(arr[i].as_double());
                    } 
                    else if(arr[i].is_float())
                    {
                        row.push_back(arr[i].as_float());
                    }
                    else if(arr[i].is_int())
                    {
                        row.push_back(arr[i].as_int());
                    }
                    else if(arr[i].is_ulonglong())
                    {
                        row.push_back(arr[i].as_ulonglong());
                    }
                }
                result_set["rows"].push_back(row);
            }
        }
        else if (cursor.get_cursor_status() == MYSQL_STATUS_OK)
        {
            result_set["status"] = "OK";
            result_set["affected_rows"] = cursor.get_affected_rows();
            result_set["warnings"] = cursor.get_warnings();
            result_set["insert_id"] = cursor.get_insert_id();
            result_set["info"] = cursor.get_info();
        }
        json["result_set"].push_back(result_set);
    } while (cursor.next_result_set());

    if (mysql_resp->get_packet_type() == MYSQL_PACKET_ERROR)
    {
        json["errcode"] = mysql_task->get_resp()->get_error_code();
        json["errmsg"] = mysql_task->get_resp()->get_error_msg();
    }
    else if (mysql_resp->get_packet_type() == MYSQL_PACKET_OK) 
    {
        json["status"] = "OK";
        json["affected_rows"] = mysql_task->get_resp()->get_affected_rows();
        json["warnings"] = mysql_task->get_resp()->get_warnings();
        json["insert_id"] = mysql_task->get_resp()->get_last_insert_id();
        json["info"] = mysql_task->get_resp()->get_info();
    }
    return json;
}

void mysql_callback(WFMySQLTask *mysql_task)
{
    Json json = mysql_concat_json_res(mysql_task);
    auto *server_resp = static_cast<HttpResp *>(mysql_task->user_data);
    server_resp->String(json.dump());
}

} // namespace wfrest


HttpReq::HttpReq() : req_data_(new ReqData)
{}

HttpReq::~HttpReq()
{
    delete req_data_;
}

std::string &HttpReq::body() const
{
    if (req_data_->body.empty())
    {
        std::string content = protocol::HttpUtil::decode_chunked_body(this);

        const std::string &header = this->header("Content-Encoding");
        int status = StatusOK;
        if (header.find("gzip") != std::string::npos)
        {
            status = Compressor::ungzip(&content, &req_data_->body);
        } else if (header.find("br") != std::string::npos)
        {
            status = Compressor::unbrotli(&content, &req_data_->body);
        } else
        {
            status = StatusNoUncomrpess;
        }
        if(status != StatusOK)
        {
            req_data_->body = std::move(content);
        }
    }
    return req_data_->body;
}

std::map<std::string, std::string> &HttpReq::form_kv() const
{
    if (content_type_ == APPLICATION_URLENCODED && req_data_->form_kv.empty())
    {
        StringPiece body_piece(this->body());
        req_data_->form_kv = Urlencode::parse_post_kv(body_piece);
    }
    return req_data_->form_kv;
}

Form &HttpReq::form() const
{
    if (content_type_ == MULTIPART_FORM_DATA && req_data_->form.empty())
    {
        StringPiece body_piece(this->body());

        req_data_->form = multi_part_.parse_multipart(body_piece);
    }
    return req_data_->form;
}

Json &HttpReq::json() const
{
    if (content_type_ == APPLICATION_JSON && req_data_->json.empty())
    {
        const std::string &body_content = this->body();
        if (!Json::accept(body_content))
        {
            return req_data_->json;
            // todo : how to let user know the error ?
        }
        req_data_->json = Json::parse(body_content);
    }
    return req_data_->json;
}

const std::string &HttpReq::param(const std::string &key) const
{
    if (route_params_.count(key))
        return route_params_.at(key);
    else
        return string_not_found;
}

bool HttpReq::has_param(const std::string &key) const
{
    return route_params_.count(key) > 0;
}

const std::string &HttpReq::query(const std::string &key) const
{
    if (query_params_.count(key))
        return query_params_.at(key);
    else
        return string_not_found;
}

const std::string &HttpReq::default_query(const std::string &key, const std::string &default_val) const
{
    if (query_params_.count(key))
        return query_params_.at(key);
    else
        return default_val;
}

bool HttpReq::has_query(const std::string &key) const
{
    if (query_params_.find(key) != query_params_.end())
    {
        return true;
    } else
    {
        return false;
    }
}

void HttpReq::fill_content_type()
{
    const std::string &content_type_str = header("Content-Type");
    content_type_ = ContentType::to_enum(content_type_str);

    if (content_type_ == MULTIPART_FORM_DATA)
    {
        // if type is multipart form, we reserve the boudary first
        const char *boundary = strstr(content_type_str.c_str(), "boundary=");
        if (boundary == nullptr)
        {
            return;
        }
        boundary += strlen("boundary=");
        StringPiece boundary_piece(boundary);

        StringPiece boundary_str = StrUtil::trim_pairs(boundary_piece, R"(""'')");
        multi_part_.set_boundary(boundary_str.as_string());
    }
}

const std::string &HttpReq::header(const std::string &key) const
{
    const auto it = headers_.find(key);

    if (it == headers_.end() || it->second.empty())
        return string_not_found;

    return it->second[0];
}

bool HttpReq::has_header(const std::string &key) const
{
    return headers_.count(key) > 0;
}

void HttpReq::fill_header_map()
{
    http_header_cursor_t cursor;
    struct protocol::HttpMessageHeader header;

    http_header_cursor_init(&cursor, this->get_parser());
    while (http_header_cursor_next(&header.name, &header.name_len,
                                   &header.value, &header.value_len,
                                   &cursor) == 0)
    {
        std::string key(static_cast<const char *>(header.name), header.name_len);

        headers_[key].emplace_back(static_cast<const char *>(header.value), header.value_len);
    }

    http_header_cursor_deinit(&cursor);
}

const std::map<std::string, std::string> &HttpReq::cookies() const
{
    if (cookies_.empty() && this->has_header("Cookie"))
    {
        const std::string &cookie = this->header("Cookie");
        StringPiece cookie_piece(cookie);
        cookies_ = std::move(HttpCookie::split(cookie_piece));
    }
    return cookies_;
}

const std::string &HttpReq::cookie(const std::string &key) const
{
    if(cookies_.empty()) 
    {
        this->cookies();
    }
    if(cookies_.find(key) != cookies_.end())
    {
        return cookies_[key];
    }
    return string_not_found;
}

HttpReq::HttpReq(HttpReq&& other)
    : HttpRequest(std::move(other)),
    content_type_(other.content_type_),
    route_match_path_(std::move(other.route_match_path_)),
    route_full_path_(std::move(other.route_full_path_)),
    route_params_(std::move(other.route_params_)),
    query_params_(std::move(other.query_params_)),
    cookies_(std::move(other.cookies_)),
    multi_part_(std::move(other.multi_part_)),
    headers_(std::move(other.headers_)),
    parsed_uri_(std::move(other.parsed_uri_))
{
    req_data_ = other.req_data_;
    other.req_data_ = nullptr;
}

HttpReq &HttpReq::operator=(HttpReq&& other)
{
    HttpRequest::operator=(std::move(other));
    content_type_ = other.content_type_;

    req_data_ = other.req_data_;
    other.req_data_ = nullptr;

    route_match_path_ = std::move(other.route_match_path_);
    route_full_path_ = std::move(other.route_full_path_);
    route_params_ = std::move(other.route_params_);
    query_params_ = std::move(other.query_params_);
    cookies_ = std::move(other.cookies_);
    multi_part_ = std::move(other.multi_part_);
    headers_ = std::move(other.headers_);
    parsed_uri_ = std::move(other.parsed_uri_);

    return *this;
}

void HttpResp::String(const std::string &str)
{
    auto *compress_data = new std::string;
    int ret = this->compress(&str, compress_data);
    if(ret != StatusOK)   
    {
        this->append_output_body(static_cast<const void *>(str.c_str()), str.size());
    } else 
    {
        this->append_output_body_nocopy(compress_data->c_str(), compress_data->size());
        task_of(this)->add_callback([compress_data](HttpTask *) { delete compress_data; });
    }
}

void HttpResp::String(std::string &&str)
{
    auto *data = new std::string;
    int ret = this->compress(&str, data);
    if(ret != StatusOK)
    {   
        *data = std::move(str);
    } 
    this->append_output_body_nocopy(data->c_str(), data->size());
    task_of(this)->add_callback([data](HttpTask *) { delete data; });
}

int HttpResp::compress(const std::string * const data, std::string *compress_data)
{
    int status = StatusOK;
    if (headers.find("Content-Encoding") != headers.end())
    {
        if (headers["Content-Encoding"].find("gzip") != std::string::npos)
        {
            status = Compressor::gzip(data, compress_data);
        } else if (headers["Content-Encoding"].find("br") != std::string::npos)
        {
            status = Compressor::brotli(data, compress_data);
        }
    } else 
    {
        status = StatusNoComrpess;
    }
    return status;
}

void HttpResp::File(const std::string &path)
{
    HttpFile::send_file(path, 0, -1, this);
}

void HttpResp::File(const std::string &path, size_t start)
{
    HttpFile::send_file(path, start, -1, this);
}

void HttpResp::File(const std::string &path, size_t start, size_t end)
{
    HttpFile::send_file(path, start, end, this);
}

void HttpResp::File(const std::vector<std::string> &path_list)
{
    headers["Content-Type"] = "multipart/form-data";
    for (int i = 0; i < path_list.size(); i++)
    {
        HttpFile::send_file_for_multi(path_list, i, this);
    }
}

void HttpResp::set_status(int status_code)
{
    protocol::HttpUtil::set_response_status(this, status_code);
}

void HttpResp::Save(const std::string &file_dst, const std::string &content)
{
    HttpFile::save_file(file_dst, content, this);
}

void HttpResp::Save(const std::string &file_dst, std::string &&content)
{
    HttpFile::save_file(file_dst, std::move(content), this);
}

void HttpResp::Json(const ::Json &json)
{
    // The header value itself does not allow for multiple values, 
    // and it is also not allowed to send multiple Content-Type headers
    // https://stackoverflow.com/questions/5809099/does-the-http-protocol-support-multiple-content-types-in-response-headers
    this->headers["Content-Type"] = "application/json";
    this->String(json.dump());
}

void HttpResp::Json(const std::string &str)
{
    if (!Json::accept(str))
    {
        std::string err = R"({"errmsg" : "invalid json syntax"})";
        this->Json(err);
        return;
    }
    this->headers["Content-Type"] = "application/json";
    this->String(str);
}

void HttpResp::set_compress(const enum Compress &compress)
{
    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Encoding
    headers["Content-Encoding"] = compress_method_to_str(compress);
}

int HttpResp::get_state() const
{
    HttpServerTask *server_task = task_of(this);
    return server_task->get_state();   
}

int HttpResp::get_error() const
{
    HttpServerTask *server_task = task_of(this);
    return server_task->get_error();   
}

void HttpResp::Http(const std::string &url, int redirect_max, size_t size_limit)
{
    HttpServerTask *server_task = task_of(this);
    HttpReq *server_req = server_task->get_req();
    WFHttpTask *http_task = WFTaskFactory::create_http_task(url, 
                                                            redirect_max, 
                                                            0, 
                                                            proxy_http_callback);
    auto *proxy_ctx = new ProxyCtx;
    proxy_ctx->url = url;
    proxy_ctx->server_task = server_task;
    proxy_ctx->is_keep_alive = server_req->is_keep_alive();
    http_task->user_data = proxy_ctx;

    const void *body;
	size_t len;             

	server_req->set_request_uri(url);
	server_req->get_parsed_body(&body, &len);
	server_req->append_output_body_nocopy(body, len);
	*http_task->get_req() = std::move(*server_req);  

    http_task->get_resp()->set_size_limit(size_limit);
	**server_task << http_task;
}

void HttpResp::MySQL(const std::string &url, const std::string &sql)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, mysql_callback);
    mysql_task->get_req()->set_query(sql);
    mysql_task->user_data = this;
    HttpServerTask *server_task = task_of(this);
    **server_task << mysql_task;
}

void HttpResp::MySQL(const std::string &url, const std::string &sql, const MySQLJsonFunc &func)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, 
    [func](WFMySQLTask *mysql_task)
    {
        ::Json json = mysql_concat_json_res(mysql_task);
        func(&json);
    });

    mysql_task->get_req()->set_query(sql);
    HttpServerTask *server_task = task_of(this);
    **server_task << mysql_task;
}

void HttpResp::MySQL(const std::string &url, const std::string &sql, const MySQLFunc &func)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, 
    [func](WFMySQLTask *mysql_task)
    {
        if (mysql_task->get_state() != WFT_STATE_SUCCESS)
        {
            std::string errmsg = WFGlobal::get_error_string(mysql_task->get_state(),
                                                mysql_task->get_error());
            auto *server_resp = static_cast<HttpResp *>(mysql_task->user_data);
            server_resp->String(std::move(errmsg));
            return;
        }
        MySQLResponse *mysql_resp = mysql_task->get_resp();
        MySQLResultCursor cursor(mysql_resp);
        func(&cursor);
    });
    mysql_task->get_req()->set_query(sql);
    mysql_task->user_data = this;
    HttpServerTask *server_task = task_of(this);
    **server_task << mysql_task;
}

HttpResp::HttpResp(HttpResp&& other)
    : HttpResponse(std::move(other)),
    headers(std::move(other.headers)),
    cookies_(std::move(other.cookies_))
{
    user_data = other.user_data;
    other.user_data = nullptr;
}

HttpResp &HttpResp::operator=(HttpResp&& other)
{
    HttpResponse::operator=(std::move(other));
    headers = std::move(other.headers);
    user_data = other.user_data;
    other.user_data = nullptr;
    cookies_ = std::move(other.cookies_);
    return *this;
}


