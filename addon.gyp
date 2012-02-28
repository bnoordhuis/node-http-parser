{
  'targets': [
    {
      'target_name': 'http-parser',
      'sources': [ 'src/http-parser.cc' ],
      'dependencies': [ 'deps/http-parser/http_parser.gyp:http_parser' ],
    }
  ]
}
