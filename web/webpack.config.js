// Bundles each page (src/<page>.html + .js + .css) into one self-contained, minified HTML file
// and gzips it: dist/<page>.html.gz is what the firmware embeds and serves.
const path = require('path');
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
        const file = tag.attributes.src || tag.attributes.href;
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

module.exports = (env, argv) => ({
  entry: Object.fromEntries(PAGES.map(p => [p, `./src/${p}.js`])),
  output: {
    path: path.resolve(env.outDir || 'dist'),
    filename: '[name].js',
    clean: true,
  },
  devtool: false,
  module: {
    rules: [{ test: /\.css$/, use: [MiniCssExtractPlugin.loader, 'css-loader'] }],
  },
  optimization: {
    minimizer: ['...', new CssMinimizerPlugin()],
  },
  performance: { hints: false },
  plugins: [
    new MiniCssExtractPlugin({ filename: '[name].css' }),
    ...PAGES.map(p => new HtmlWebpackPlugin({
      template: `./src/${p}.html`,
      filename: `${p}.html`,
      chunks: [p],
      inject: 'body',
      scriptLoading: 'blocking',
      minify: argv.mode === 'production' && {
        collapseWhitespace: true, removeComments: true, minifyCSS: true, minifyJS: true,
      },
    })),
    new InlineAssetsPlugin(),
    new CompressionPlugin({ test: /\.html$/, algorithm: 'gzip', compressionOptions: { level: 9 } }),
  ],
});
