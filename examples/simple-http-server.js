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

var assert = require('assert');
var events = require('events');
var util = require('util');
var net = require('net');

try {
  var http_parser = require('../out/Debug/http-parser');
}
catch (e) {
  var http_parser = require('../out/Release/http-parser');
}

function Context() {
  this.reset();
}
util.inherits(Context, events.EventEmitter);

Context.prototype.reset = function() {
  this.url = '';
  this.headers = [];
  this.headerName = '';
  this.headerValue = '';
};

Context.prototype.on_message_begin = function(parser) {
  this.emit('message_begin');
};

Context.prototype.on_url = function(parser, buf, start, len) {
  this.url += buf.slice(start, start + len);
};

Context.prototype.on_header_field = function(parser, buf, start, len) {
  this.headerName += buf.slice(start, start + len);
  if (!this.headerValue) return;
  this.headers.push(this.headerValue);
  this.headerValue = '';
};

Context.prototype.on_header_value = function(parser, buf, start, len) {
  this.headerValue += buf.slice(start, start + len);
  if (!this.headerName) return;
  this.headers.push(this.headerName);
  this.headerName = '';
};

Context.prototype.on_headers_complete = function(parser) {
  this.emit('headers_complete');
  if (!this.headerValue) return;
  this.headers.push(this.headerValue);
  this.headerValue = '';
};

Context.prototype.on_body = function(parser, buf, start, len) {
  this.emit('data', buf.slice(start, start + len));
};

Context.prototype.on_message_complete = function(parser) {
  this.emit('message_complete');
  this.reset();
};

var server = net.createServer(function(conn) {
  var ctx = new Context, parser = new http_parser.Parser(ctx);

  conn.on('data', function(buf) {
    var r = parser.execute(buf, 0, buf.length);
    if (r != buf.length) throw new Error('Parse error');
  });

  ctx.on('headers_complete', function() {
    conn.write(
      'HTTP/1.1 200 OK\r\n' +
      'Transfer-Encoding: chunked\r\n' +
      'Content-Type: text/plain\r\n' +
      'Connection: close\r\n' +
      'Date: ' + Date() + '\r\n' +
      '\r\n');
  });

  ctx.on('data', function(buf) {
    conn.write(buf.length.toString(16) + '\r\n');
    conn.write(buf + '\r\n');
  });

  ctx.on('message_complete', function() {
    conn.write('0\r\n\r\n'); // final chunk
    conn.destroy();
  });
});

server.listen(8000, '127.0.0.1');
console.log('Listening on http://127.0.0.1:8000/');
