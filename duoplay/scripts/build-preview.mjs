// Builds the single-page preview of the web app (no bridge needed): one HTML file with the
// app's CSS and JS inlined, plus the HDR check clip beside it.
//   node scripts/build-preview.mjs [outDir]
import { build } from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = process.argv[2] || join(root, 'dist', 'preview');
mkdirSync(join(out, 'media'), { recursive: true });

const js = await build({ entryPoints: [join(root, 'web/js/main.js')], bundle: true, format: 'iife', target: 'safari17', write: false, minifySyntax: true });
const index = readFileSync(join(root, 'web/index.html'), 'utf8');
const launchCss = index.match(/<style>([\s\S]*?)<\/style>/)[1];
const appCss = readFileSync(join(root, 'web/app.css'), 'utf8');
const body = index.match(/<body[^>]*>([\s\S]*?)<script type="module"/)[1];

const html = `<title>DuoPlay</title>
<meta name="claude:viewport-fit-sides" content="cover">
<style>${launchCss}
${appCss}
/* The preview host already pads the page for the phone's safe areas. */
.home { padding-top: 14px; padding-bottom: 28px; }
</style>
${body}
<script>window.DUOPLAY_PREVIEW = true;</script>
<script>${js.outputFiles[0].text.replace(/<\/script/gi, '<\\/script')}</script>
`;
writeFileSync(join(out, 'index.html'), html);
copyFileSync(join(root, 'web/media/hdr-check.mp4'), join(out, 'media/hdr-check.mp4'));
console.log(`preview: ${join(out, 'index.html')} (${(html.length / 1024).toFixed(0)} KB)`);
