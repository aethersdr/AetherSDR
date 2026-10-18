// @ts-check
// AetherSDR user documentation: https://docs.aethersdr.com
// The build is also the link check: a broken page link or anchor fails it.

import {themes as prismThemes} from 'prism-react-renderer';
import {createRequire} from 'node:module';
import {writeFile} from 'node:fs/promises';
import path from 'node:path';

// The release the Stable docs describe. Written by
// tools/docs/snapshot_stable.py with the snapshot in versioned_docs/.
const require = createRequire(import.meta.url);
const stable = require('./stable-version.json');

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'AetherSDR Docs',
  tagline: 'Native. Open. Yours.',
  favicon: 'img/favicon.png',

  url: 'https://docs.aethersdr.com',
  baseUrl: '/',
  trailingSlash: false,

  organizationName: 'aethersdr',
  projectName: 'AetherSDR',

  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',

  markdown: {
    // .md files are parsed as CommonMark, .mdx as MDX. The pages are
    // converted from the GitHub wiki, where prose like `<name>` and `{x}`
    // is ordinary text; CommonMark keeps it that way.
    format: 'detect',
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  // Translations live in i18n/<locale>/; see docs/translating-the-docs.md.
  // To add a language, list it here and run
  // `npm run write-translations -- --locale <locale>`, for example:
  //   locales: ['en', 'fr'],
  //   localeConfigs: {fr: {label: 'Français'}},
  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  plugins: [
    // The printable manual is not deployed with the site: it is larger than
    // the 25 MiB a Cloudflare Pages file may be. /AetherSDR-Manual.pdf, the
    // link in the site and in the app's Help menu, is a redirect to the copy
    // attached to the stable release (docs.yml makes and signs it).
    () => ({
      name: 'manual-pdf-redirect',
      async postBuild({outDir}) {
        const tag = `v${stable.label}`;
        const pdf = `https://github.com/aethersdr/AetherSDR/releases/download/${tag}/AetherSDR-Manual-${tag}.pdf`;
        await writeFile(path.join(outDir, '_redirects'), `/AetherSDR-Manual.pdf ${pdf} 302\n`);
      },
    }),
  ],

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          routeBasePath: '/',
          sidebarPath: './sidebars.js',
          editUrl: 'https://github.com/aethersdr/AetherSDR/edit/main/docs/user/',
          // A page of the stable snapshot is fixed; its edit link opens the
          // same page in docs/, which is where a fix goes.
          editCurrentVersion: true,
          // Two versions: the stable snapshot (versioned_docs/version-stable,
          // served at /) and the current docs/, which track main (at /next/).
          lastVersion: 'stable',
          versions: {
            current: {
              label: 'Next (main)',
              banner: 'unreleased',
            },
            stable: {
              label: stable.label,
              banner: 'none',
            },
          },
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      }),
    ],
  ],

  themes: [
    [
      '@easyops-cn/docusaurus-search-local',
      /** @type {import('@easyops-cn/docusaurus-search-local').PluginOptions} */
      ({
        hashed: true,
        indexDocs: true,
        indexBlog: false,
        indexPages: false,
        docsRouteBasePath: '/',
        language: ['en'],
        highlightSearchTermsOnTargetPage: true,
        explicitSearchResultPath: true,
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      colorMode: {
        defaultMode: 'dark',
        disableSwitch: false,
        respectPrefersColorScheme: false,
      },
      navbar: {
        title: 'AetherSDR',
        logo: {
          alt: 'AetherSDR logo',
          src: 'img/logo.png',
        },
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'docs',
            position: 'left',
            label: 'Docs',
          },
          {
            href: 'https://github.com/aethersdr/AetherSDR/releases/latest',
            label: 'Download',
            position: 'left',
          },
          {to: '/log-analyzer', label: 'Log Analyzer', position: 'left'},
          {
            // Redirected to the stable release's PDF (the plugin above).
            // pathname:// keeps the link checker off it: it is a file, not a
            // page.
            href: 'pathname:///AetherSDR-Manual.pdf',
            label: 'PDF manual',
            position: 'left',
          },
          {
            type: 'docsVersionDropdown',
            position: 'right',
          },
          {
            href: 'https://www.aethersdr.com',
            label: 'Website',
            position: 'right',
          },
          {
            href: 'https://github.com/aethersdr/AetherSDR',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {label: 'Home', to: '/'},
              {label: 'Installation', to: '/installation'},
              {label: 'Troubleshooting', to: '/troubleshooting'},
              {label: 'Log Analyzer', to: '/log-analyzer'},
              {label: 'PDF manual', href: 'pathname:///AetherSDR-Manual.pdf'},
            ],
          },
          {
            title: 'Project',
            items: [
              {label: 'Download', href: 'https://github.com/aethersdr/AetherSDR/releases/latest'},
              {label: 'Website', href: 'https://www.aethersdr.com'},
              {label: 'GitHub', href: 'https://github.com/aethersdr/AetherSDR'},
              {label: 'Report an issue', href: 'https://github.com/aethersdr/AetherSDR/issues'},
            ],
          },
        ],
        copyright: `AetherSDR is free software, licensed under the <a href="https://github.com/aethersdr/AetherSDR/blob/main/LICENSE">GNU General Public License v3.0</a>. Documentation built with Docusaurus.`,
      },
      prism: {
        theme: prismThemes.github,
        darkTheme: prismThemes.dracula,
        additionalLanguages: ['bash', 'powershell', 'json', 'ini', 'cmake'],
      },
    }),
};

export default config;
