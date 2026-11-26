# yaml 2.8.1

The browser ES-module build (`browser/dist/` of the npm package `yaml@2.8.1`, ISC, by Eemeli
Aro, https://github.com/eemeli/yaml), as published, unchanged. It is the version dax-sql's own
tests parse Apache Ossie models with. The extension embeds it (`cmake/embed_js.cmake`) so that
the glue can read a `.yaml` model before dax-sql's `formats/ossie.js` reads the parsed object.

To update: `npm pack yaml@<version>` (dax-sql's Pack workflow, on a runner), and replace `dist/`
with the tarball's `package/browser/dist/`.
