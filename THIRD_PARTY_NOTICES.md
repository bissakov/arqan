# Third-party notices

This distribution contains the following vendored components. The complete
license and notice texts are included at the paths listed below.

## Lexbor

Lexbor 3.0.0 is used for HTML parsing and is distributed under the Apache
License 2.0. See:

- `vendor/lexbor/LICENSE`
- `vendor/lexbor/NOTICE`

## Tree-sitter

The Tree-sitter 0.26.11 C runtime is distributed under the MIT License. See
`vendor/tree-sitter/licenses/tree-sitter.txt`.

The bundled generated grammars and scanners are distributed under their
upstream licenses:

| Grammar | Complete license text |
| --- | --- |
| Bash | `vendor/tree-sitter/licenses/bash.txt` |
| C | `vendor/tree-sitter/licenses/c.txt` |
| C++ | `vendor/tree-sitter/licenses/cpp.txt` |
| Go | `vendor/tree-sitter/licenses/go.txt` |
| JavaScript | `vendor/tree-sitter/licenses/javascript.txt` |
| JSON | `vendor/tree-sitter/licenses/json.txt` |
| Python | `vendor/tree-sitter/licenses/python.txt` |
| Rust | `vendor/tree-sitter/licenses/rust.txt` |
| TOML | `vendor/tree-sitter/licenses/toml.txt` |
| TypeScript and TSX | `vendor/tree-sitter/licenses/typescript.txt` |
| YAML | `vendor/tree-sitter/licenses/yaml.txt` |

Tree-sitter's Unicode support includes data and runtime headers derived from
ICU. Their Unicode/ICU license and notices are in
`vendor/tree-sitter/runtime/unicode/LICENSE`.

## Kanagawa

The colour values of the `kanagawa-wave`, `kanagawa-dragon` and
`kanagawa-lotus` themes in `src/theme.c` come from kanagawa.nvim
(github.com/rebelot/kanagawa.nvim), distributed under the MIT License. The
`kanagawa-lotus` text colours are darkened for contrast.

```
MIT License

Copyright (c) 2021 Tommaso Laurenzi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
