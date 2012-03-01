/* Copyright (c) 2012 Ben Noordhuis <info@bnoordhuis.nl>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "http_parser.h"
#include "node_buffer.h"
#include "node.h"
#include "v8.h"

#include <stdlib.h>
#include <assert.h>

#define offset_of(type, member) \
  ((intptr_t) ((char *) &(((type *) 256)->member) - 256))

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offset_of(type, member)))

#define ARRAY_SIZE(a) (sizeof((a)) / sizeof((a)[0]))

#define THROW(s) ThrowException(Exception::Error(String::New(s)))

#define HTTP_CB_DATA_MAP(X) \
  X(1, on_url) \
  X(2, on_header_field) \
  X(3, on_header_value) \
  X(5, on_body)

#define HTTP_CB_NODATA_MAP(X) \
  X(0, on_message_begin) \
  X(4, on_headers_complete) \
  X(6, on_message_complete)

#define HTTP_CB_MAP(X) \
  HTTP_CB_NODATA_MAP(X) \
  HTTP_CB_DATA_MAP(X)

using namespace v8;

namespace
{

struct Parser: public node::ObjectWrap
{
  Persistent<Function> callbacks_[7];
  Persistent<Object> target_;
  Handle<Object> buffer_;
  http_parser parser_;
  virtual ~Parser();
  static Handle<Value> New(const Arguments& args);
};

http_parser_settings settings;
Persistent<FunctionTemplate> clazz;

#define X(num, name) Persistent<String> name##_sym;
HTTP_CB_MAP(X)
#undef X

Parser::~Parser() {
#define X(num, name) \
  callbacks_[num].Clear(); \
  callbacks_[num].Dispose();
  HTTP_CB_MAP(X)
#undef X
}

template <int num>
static int http_cb(http_parser* parser)
{
  Parser* self = container_of(parser, Parser, parser_);
  Handle<Function> cb = self->callbacks_[num];
  if (cb.IsEmpty()) return 0;
  HandleScope scope;
  TryCatch tc;
  Handle<Value> argv[] = { self->handle_ };
  Local<Value> r = cb->Call(self->target_, ARRAY_SIZE(argv), argv);
  if (tc.HasCaught()) node::FatalException(tc);
  if (r.IsEmpty()) return -1;
  return r->Int32Value();
}

template <int num>
static int http_cb(http_parser* parser, const char* data, size_t size)
{
  Parser* self = container_of(parser, Parser, parser_);
  assert(!self->buffer_.IsEmpty());
  Handle<Function> cb = self->callbacks_[num];
  if (cb.IsEmpty()) return 0;
  HandleScope scope;
  const char* buf = node::Buffer::Data(self->buffer_);
  size_t buflen = node::Buffer::Length(self->buffer_);
  assert(data >= buf);
  assert(data + size <= buf + buflen);
  Handle<Value> argv[] = {
    self->handle_,
    self->buffer_,
    Integer::NewFromUnsigned(data - buf),
    Integer::NewFromUnsigned(size)
  };
  TryCatch tc;
  Local<Value> r = cb->Call(self->target_, ARRAY_SIZE(argv), argv);
  if (tc.HasCaught()) node::FatalException(tc);
  if (r.IsEmpty()) return -1;
  return r->Int32Value();
}

void Rebind(Parser* self, Handle<Value> val)
{
  self->target_ = Persistent<Object>::New(val->ToObject());
#define X(num, name) \
  do { \
    Local<Value> val = self->target_->Get(name##_sym); \
    if (val->IsFunction()) \
      self->callbacks_[num] = Persistent<Function>::New(val.As<Function>()); \
    else \
      self->callbacks_[num].Clear(); \
  } \
  while (0);
  HTTP_CB_MAP(X)
#undef X
}

Handle<Value> Parser::New(const Arguments& args)
{
  HandleScope scope;
  assert(args.IsConstructCall());
  if (!args[0]->IsObject()) return THROW("Argument must be an object");
  Parser* self = new Parser;
  self->Wrap(args.This());
  http_parser_type type = static_cast<http_parser_type>(args[1]->Uint32Value());
  http_parser_init(&self->parser_, type);
  Rebind(self, args[0]);
  return scope.Close(self->handle_);
}

#define UNWRAP() \
  assert(clazz->HasInstance(args.This())); \
  Parser* self = static_cast<Parser*>( \
      args.This()->GetPointerFromInternalField(0));

Handle<Value> Execute(const Arguments& args)
{
  UNWRAP();
  HandleScope scope;
  Local<Object> obj = args[0]->ToObject();
  const char* buf = node::Buffer::Data(obj);
  size_t buflen = node::Buffer::Length(obj);
  size_t start = args[1]->Uint32Value();
  size_t len = args[2]->Uint32Value();
  if (start >= buflen || start + len > buflen || start > start + len)
    return THROW("Out of bounds");
  self->buffer_ = obj;
  size_t r = http_parser_execute(&self->parser_, &settings, buf + start, len);
  self->buffer_.Clear();
  return scope.Close(Integer::NewFromUnsigned(r));
}

template <int action>
Handle<Value> Pause(const Arguments& args)
{
  UNWRAP();
  http_parser_pause(&self->parser_, action);
  return Undefined();
}

Handle<Value> Rebind(const Arguments& args)
{
  HandleScope scope;
  if (!args[0]->IsObject()) return THROW("Argument must be an object");
  UNWRAP();
  Rebind(self, args[0]);
  return Undefined();
}

Handle<Value> Reset(const Arguments& args)
{
  UNWRAP();
  http_parser_type type = static_cast<http_parser_type>(args[0]->Uint32Value());
  http_parser_init(&self->parser_, type);
  return Undefined();
}

Handle<Value> StrError(const Arguments& args)
{
  HandleScope scope;

  switch (args[0]->Int32Value()) {
#define X(code, err) case HPE_##code: return scope.Close(String::New(#err));
  HTTP_ERRNO_MAP(X)
#undef X
  }

  return scope.Close(String::New("unknown error"));
}

#define X(name) \
  Handle<Value> name##_prop(Local<String> name, const AccessorInfo& args) \
  { \
    HandleScope scope; \
    UNWRAP(); \
    return scope.Close(Integer::New(self->parser_.name)); \
  }
  X(http_major)
  X(http_minor)
  X(status_code)
  X(method)
  X(http_errno)
  X(upgrade)
#undef X

extern "C" void init(Handle<Object> obj)
{
  HandleScope scope;

#define X(num, name) \
  name##_sym = Persistent<String>::New(String::NewSymbol(#name)); \
  settings.name = http_cb<num>;
  HTTP_CB_MAP(X)
#undef X

  Local<FunctionTemplate> t = FunctionTemplate::New(Parser::New);
  clazz = Persistent<FunctionTemplate>::New(t);

  t->InstanceTemplate()->SetInternalFieldCount(1);
  t->SetClassName(String::New("Parser"));

  Local<ObjectTemplate> proto = t->PrototypeTemplate();
  proto->SetInternalFieldCount(1);
  proto->Set(String::New("execute"),
             FunctionTemplate::New(Execute)->GetFunction());
  proto->Set(String::New("pause"),
             FunctionTemplate::New(Pause<1>)->GetFunction());
  proto->Set(String::New("unpause"),
             FunctionTemplate::New(Pause<0>)->GetFunction());
  proto->Set(String::New("rebind"),
             FunctionTemplate::New(Rebind)->GetFunction());
  proto->Set(String::New("reset"),
             FunctionTemplate::New(Reset)->GetFunction());

#define X(name) proto->SetAccessor(String::New(#name), name##_prop);
  X(http_major)
  X(http_minor)
  X(status_code)
  X(method)
  X(http_errno)
  X(upgrade)
#undef X

#define X(num, name) \
  obj->Set(String::NewSymbol("M_" # name), Integer::New(num));
  HTTP_METHOD_MAP(X)
#undef X

#define X(code, msg) \
  obj->Set(String::NewSymbol("E_" # code), Integer::New(HPE_##code));
  HTTP_ERRNO_MAP(X)
#undef X

  obj->Set(String::NewSymbol("Parser"), t->GetFunction());
  obj->Set(String::NewSymbol("HTTP_BOTH"), Integer::New(HTTP_BOTH));
  obj->Set(String::NewSymbol("HTTP_REQUEST"), Integer::New(HTTP_REQUEST));
  obj->Set(String::NewSymbol("HTTP_RESPONSE"), Integer::New(HTTP_RESPONSE));
  obj->Set(String::NewSymbol("strerror"),
           FunctionTemplate::New(StrError)->GetFunction());
}

} // anonymous namespace
