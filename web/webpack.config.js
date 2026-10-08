// Bundles each page (src/<page>/: <page>.html shell + main.jsx Preact app + CSS) into one
// self-contained, minified, gzipped HTML file. Output (in <outDir>, default dist/):
//   www/<page>.html.gz, www/manifest.json   the bridge's "www" SPIFFS partition (and the
//                                           firmware's built-in fallback copy)
//   dmx_bridge_www.tar                      the same files as one package, to upload on the
//                                           settings page (Firmware card) without reflashing
const path = require('path');
const { execSync } = require('child_process');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const MiniCssExtractPlugin = require('mini-css-extract-plugin');
const CssMinimizerPlugin = require('css-minimizer-webpack-plugin');
const CompressionPlugin = require('compression-webpack-plugin');

const PAGES = ['index', 'console'];

// Replace <script src>/<link href> tags with the file contents, then drop those files.
class InlineAssetsPlugin {
  apply(compiler) {
    compiler.hooks.compilation.tap('InlineAssets', compilation => {
      const inlined = new Set();
      const inline = tag => {
        const file = (tag.attributes.src || tag.attributes.href || '').replace(/^\//, '');
        const asset = file && compilation.getAsset(file);
        if (!asset) return tag;
        inlined.add(file);
        // Keep a literal "</script>" or "</style>" inside the code from ending the tag.
        const code = asset.source.source().toString().replace(/<\/(script|style)/gi, '<\\/$1');
        return tag.tagName === 'script'
          ? { tagName: 'script', voidTag: false, meta: tag.meta, attributes: {}, innerHTML: code }
          : { tagName: 'style', voidTag: false, meta: tag.meta, attributes: {}, innerHTML: code };
      };
      HtmlWebpackPlugin.getCompilationHooks(compilation).alterAssetTagGroups.tap('InlineAssets', data => {
        data.headTags = data.headTags.map(inline);
        data.bodyTags = data.bodyTags.map(inline);
        return data;
      });
      compilation.hooks.processAssets.tap(
        { name: 'InlineAssets', stage: compiler.webpack.Compilation.PROCESS_ASSETS_STAGE_SUMMARIZE },
        () => inlined.forEach(f => compilation.deleteAsset(f)));
    });
  }
}

// Minimal POSIX ustar archive: regular files only, names up to 100 bytes.
function tar(files) {
  const blocks = [];
  for (const [name, data] of files) {
    const h = Buffer.alloc(512);
    const field = (off, len, str) => h.write(str, off, len, 'ascii');
    const octal = (off, len, n) => field(off, len, n.toString(8).padStart(len - 1, '0') + '\0');
    field(0, 100, name);
    octal(100, 8, 0o644);
    octal(108, 8, 0);
    octal(116, 8, 0);
    octal(124, 12, data.length);
    octal(136, 12, Math.floor(Date.now() / 1000));
    field(148, 8, '        ');            // checksum placeholder (spaces) while summing
    field(156, 1, '0');                   // regular file
    field(257, 6, 'ustar\0');
    field(263, 2, '00');
    let sum = 0;
    for (const b of h) sum += b;
    field(148, 8, sum.toString(8).padStart(6, '0') + '\0 ');
    blocks.push(h, data, Buffer.alloc((512 - (data.length % 512)) % 512));
  }
  blocks.push(Buffer.alloc(1024));        // end of archive
  return Buffer.concat(blocks);
}

// Adds www/manifest.json and writes dmx_bridge_www.tar next to the www/ folder.
class WebPackagePlugin {
  apply(compiler) {
    compiler.hooks.thisCompilation.tap('WebPackage', compilation => {
      compilation.hooks.processAssets.tap(
        { name: 'WebPackage', stage: compiler.webpack.Compilation.PROCESS_ASSETS_STAGE_REPORT },
        () => {
          let git = '';
          try { git = execSync('git rev-parse --short HEAD', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(); } catch (e) {}
          const files = Object.keys(compilation.assets).filter(f => f.startsWith('www/')).map(f => f.slice(4)).sort();
          const manifest = { version: require('./package.json').version, git, built: new Date().toISOString(), files };
          const { RawSource } = compiler.webpack.sources;
          compilation.emitAsset('www/manifest.json', new RawSource(JSON.stringify(manifest)));
          const entries = Object.keys(compilation.assets).filter(f => f.startsWith('www/')).sort()
            .map(f => [f.slice(4), Buffer.from(compilation.assets[f].source())]);
          compilation.emitAsset('dmx_bridge_www.tar', new RawSource(tar(entries)));
        });
    });
  }
}

module.exports = (env, argv) => ({
  entry: Object.fromEntries(PAGES.map(p => [p, `./src/${p}/main.jsx`])),
  output: {
    path: path.resolve(env.outDir || 'dist'),
    filename: '[name].js',
    publicPath: '/',
    clean: true,
  },
  devtool: false,
  module: {
    rules: [
      { test: /\.css$/, use: [MiniCssExtractPlugin.loader, 'css-loader'] },
      {
        test: /\.jsx?$/,
        exclude: /node_modules/,
        loader: 'esbuild-loader',
        options: { loader: 'jsx', jsx: 'automatic', jsxImportSource: 'preact', target: 'es2020' },
      },
    ],
  },
  resolve: { extensions: ['.js', '.jsx'] },
  optimization: {
    minimizer: ['...', new CssMinimizerPlugin()],
  },
  performance: { hints: false },
  plugins: [
    new MiniCssExtractPlugin({ filename: '[name].css' }),
    ...PAGES.map(p => new HtmlWebpackPlugin({
      template: `./src/${p}/${p}.html`,
      filename: `www/${p}.html`,
      chunks: [p],
      inject: 'body',
      scriptLoading: 'blocking',
      minify: argv.mode === 'production' && {
        collapseWhitespace: true, removeComments: true, minifyCSS: true, minifyJS: true,
      },
    })),
    new InlineAssetsPlugin(),
    new CompressionPlugin({
      test: /\.html$/, algorithm: 'gzip', compressionOptions: { level: 9 }, deleteOriginalAssets: true,
    }),
    new WebPackagePlugin(),
  ],
});
