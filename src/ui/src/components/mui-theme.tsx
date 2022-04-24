/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */


import { PaletteMode } from '@mui/material';
import { createTheme, Theme } from '@mui/material/styles';
import type {
  PaletteOptions as AugmentedPaletteOptions,
  SimplePaletteColorOptions,
} from '@mui/material/styles/createPalette';

interface SyntaxPalette {
  /** Default color for tokens that don't match any other rules */
  normal: string;
  /** Scoping tokens, such as (parens), [brackets], and {braces} */
  scope: string;
  /** Tokens that separate others, such as =,.:; */
  divider: string;
  /** Tokens that have something wrong (semantic or syntax errors, etc) */
  error: string;
  // Primitives...
  boolean: string;
  number: string;
  string: string;
  nullish: string;
}

declare module '@mui/material/styles/createPalette' {
  interface TypeBackground {
    one: string;
    two: string;
    three: string;
    four: string;
    five: string;
    six: string;
  }

  interface Palette {
    foreground: {
      one: string;
      two: string;
      three: string;
      grey1: string;
      grey2: string;
      grey3: string;
      grey4: string;
      grey5: string;
      white: string;
    };
    sideBar: {
      color: string;
      colorShadow: string;
      colorShadowOpacity: number;
    };
    graph: {
      category: string[];
      diverging: string[];
      ramp: string[];
      heatmap: string[];
      primary: string;
      flamegraph: {
        kernel: string,
        java: string,
        app: string,
        k8s: string,
      };
    };
    border: {
      focused: string;
      unFocused: string;
    };
    syntax: SyntaxPalette;
  }

  interface PaletteOptions {
    foreground: {
      one: string;
      two: string;
      three: string;
      grey1: string;
      grey2: string;
      grey3: string;
      grey4: string;
      grey5: string;
      white: string;
    };
    sideBar: {
      color: string;
      colorShadow: string;
      colorShadowOpacity: number;
    };
    graph: {
      primary: string;
      category: string[];
      diverging: string[];
      ramp: string[];
      heatmap: string[];
      flamegraph: {
        kernel: string,
        java: string,
        app: string,
        k8s: string,
      };
    };
    border: {
      focused: string;
      unFocused: string;
    };
    syntax: SyntaxPalette;
  }
}

declare module '@mui/material/styles' {
  interface RoundedBorderRadius {
    small: string;
    large: string;
  }

  export interface Shape {
    borderRadius: string | number;
    leftRoundedBorderRadius: RoundedBorderRadius;
    rightRoundedBorderRadius: RoundedBorderRadius;
  }

  interface Theme {
    shape: Shape;
  }

  interface TypographyVariants {
    monospace: React.CSSProperties;
  }

  interface TypographyVariantOptions {
    monospace?: React.CSSProperties;
  }
}

declare module '@mui/material/Typography' {
  interface TypographyPropsVariantOverrides {
    monospace: true;
  }
}

declare module '@mui/material/styles/createTheme' {
  interface ThemeOptions {
    shape?: Partial<Theme['shape']>;
  }
}

// eslint-disable-next-line @typescript-eslint/explicit-module-boundary-types
export const scrollbarStyles = (theme: Theme) => {
  const commonStyle = (color: string) => ({
    borderRadius: theme.spacing(1.0),
    border: [['solid', theme.spacing(0.7), 'transparent']],
    backgroundColor: 'transparent',
    boxShadow: [['inset', 0, 0, theme.spacing(0.5), theme.spacing(1), color]],
  });
  return {
    '& ::-webkit-scrollbar': {
      width: theme.spacing(2),
      height: theme.spacing(2),
    },
    '& ::-webkit-scrollbar-track': commonStyle(theme.palette.background.one),
    '& ::-webkit-scrollbar-thumb': commonStyle(theme.palette.foreground.grey2),
    '& ::-webkit-scrollbar-corner': {
      backgroundColor: 'transparent',
    },
  };
};

export function addSyntaxToPalette(base: Omit<AugmentedPaletteOptions, 'syntax'>): AugmentedPaletteOptions {
  return {
    ...base,
    syntax: {
      normal: base.text.secondary,
      scope: base.text.secondary,
      divider: base.foreground.three,
      error: (base.error as Required<SimplePaletteColorOptions>).main,
      boolean: (base.success as Required<SimplePaletteColorOptions>).main,
      number: (base.secondary as Required<SimplePaletteColorOptions>).main,
      string: base.mode === 'dark' ? base.graph.ramp[0] : base.graph.ramp[2],
      nullish: base.foreground.three,
    },
  };
}

export const EDITOR_THEME_MAP: Record<PaletteMode, string> = {
  dark: 'vs-dark',
  light: 'vs-light',
};

// Note: not explicitly typed, as it's a messy nested partial of ThemeOptions.
// When combined with LIGHT_THEME and DARK_THEME below, however, TypeScript
// will still recognize if the merged object is missing anything.
export const COMMON_THEME = {
  shape: {
    borderRadius: '5px',
    leftRoundedBorderRadius: {
      large: '10px 0px 0px 10px',
      small: '5px 0px 0px 5px',
    },
    rightRoundedBorderRadius: {
      large: '0px 10px 10px 0px',
      small: '0px 5px 5px 0px',
    },
  },
  zIndex: {
    drawer: 1075, // Normally higher than appBar and below modal. We want it below appBar.
  },
  typography: {
    // The below rem conversions are based on a root font-size of 16px.
    h1: {
      fontSize: '2.125rem', // 34px
      fontWeight: 400,
    },
    h2: {
      fontSize: '1.5rem', // 24px
      fontWeight: 500,
    },
    h3: {
      fontSize: '1.125rem', // 18px
      fontWeight: 500,
      marginBottom: '16px',
      marginTop: '12px',
    },
    h4: {
      fontSize: '0.875rem', // 14px
      fontWeight: 500,
    },
    caption: {
      fontSize: '0.875rem', // 14px
    },
    subtitle1: {
      fontSize: '1rem', // 16px
    },
    subtitle2: {
      fontSize: '0.875rem', // 14px
      fontWeight: 400,
    },
    monospace: {
      fontFamily: '"Roboto Mono", monospace',
    },
  },
  palette: {
    border: {
      focused: '1px solid rgba(255, 255, 255, 0.2)',
      unFocused: '1px solid rgba(255, 255, 255, 0.1)',
    },
    sideBar: {
      color: '#161616',
      colorShadow: '#000000',
      colorShadowOpacity: 0.5,
    },
    common: {
      black: '#000000',
      white: '#ffffff',
    },
    primary: {
      main: '#12d6d6',
      dark: '#17aaaa',
      light: '#3ef3f3',
    },
    secondary: {
      main: '#24b2ff',
      dark: '#21a1e7',
      light: '#79d0ff',
    },
    action: {
      active: '#a6a8ae', // foreground 1.
    },
    graph: {
      category: [
        '#21a1e7', // one
        '#2ca02c', // two
        '#98df8a', // three
        '#aec7e8', // four
        '#ff7f0e', // five
        '#ffbb78', // six
      ],
      diverging: [
        '#cc0020', // left-dark
        '#e77866', // left-main
        '#f6e7e1', // left-light
        '#d6e8ed', // right-light
        '#91bfd9', // right-main
        '#1d78b5', // right-dark
      ],
      ramp: [
        '#fff48f', // info-light
        '#f0de3d', // info-main
        '#dac92f', // info-dark
        '#ffc656', // warning-light
        '#f6a609', // warning-main
        '#dc9406', // warning-dark
        '#ff5e6d', // error-main
        '#e54e5c', // error-dark
      ],
      heatmap: [
        '#d6e8ed', // light-1
        '#cee0e5', // light-2
        '#91bfd9', // main
        '#549cc6', // dark-1
        '#1d78b5', // dark-2
      ],
      primary: '#39a8f5',
    },
  },
  components: {
    MuiCssBaseline: {
      // Global styles.
      styleOverrides: {
        '#root': {
          height: '100%',
          width: '100%',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        },
        html: {
          height: '100%',
        },
        body: {
          height: '100%',
          overflow: 'hidden',
          margin: 0,
          boxSizing: 'border-box',
        },
        ':focus': {
          outline: 'none !important',
        },
        // Vega-Tooltip (included in Vega-Lite) doesn't quite get right math when constraining a tooltip to the screen.
        // Also, we sometimes use titles so long they fill the screen (like generated method signatures in flamegraphs).
        // Sticky positioning and multi-line word wrap, respectively, handle both issues with just CSS.
        '#vg-tooltip-element.vg-tooltip': {
          maxWidth: 'min(400px, 80vw)',
          width: 'fit-content',
          position: 'sticky',
          // The div#root element confuses sticky positioning without this negative margin.
          marginTop: '-100vh',
          '& > h2': {
            display: '-webkit-box',
            maxWidth: '100%',
            WebkitLineClamp: 4,
            WebkitBoxOrient: 'vertical',
            overflow: 'hidden',
            overflowWrap: 'anywhere',
          },
        },
      },
    },
    MuiMenuItem: {
      styleOverrides: {
        root: {
          width: '100%',
        },
      },
    },
  },
};

export const DARK_BASE = {
  ...COMMON_THEME,
  ...{
    palette: {
      ...COMMON_THEME.palette,
      mode: 'dark' as const,
      divider: '#272822',
      foreground: {
        one: '#b2b5bb',
        two: '#f2f2f2',
        three: '#9696a5',
        grey1: '#4a4c4f',
        grey2: '#353738',
        grey3: '#212324',
        grey4: '#596274',
        grey5: '#dbdde0',
        white: '#ffffff',
      },
      background: {
        default: '#121212',
        paper: '#121212',
        one: '#121212',
        two: '#212324',
        three: '#353535',
        four: '#161616',
        five: '#090909',
        six: '#242424',
      },
      text: {
        primary: '#e2e5ee',
        secondary: '#ffffff',
        disabled: 'rgba(226, 229, 238, 0.7)', // 70% opacity of primary
      },
      success: {
        main: '#00dba6',
        dark: '#00bd8f',
        light: '#4dffd4',
      },
      warning: {
        main: '#f6a609',
        dark: '#dc9406',
        light: '#ffc656',
      },
      info: { // Same as secondary
        main: '#24b2ff',
        dark: '#21a1e7',
        light: '#79d0ff',
      },
      error: {
        main: '#ff5e6d',
        dark: '#e54e5c',
        light: '#ff9fa8',
      },
      graph: {
        ...COMMON_THEME.palette.graph,
        flamegraph: {
          kernel: '#98df8a',
          java: '#31d0f3',
          app: '#31d0f3',
          k8s: '#4796c1',
        },
      },
    },
    components: {
      ...COMMON_THEME.components,
      MuiDivider: {
        styleOverrides: {
          root: {
            backgroundColor: '#353535', // background three.
          },
        },
      },
    },
  },
};

export const DARK_THEME = createTheme({
  ...DARK_BASE,
  palette: addSyntaxToPalette(DARK_BASE.palette),
});

export const LIGHT_BASE = {
  ...COMMON_THEME,
  ...{
    palette: {
      ...COMMON_THEME.palette,
      mode: 'light' as const,
      divider: '#dbdde0',
      foreground: {
        one: '#4f4f4f',
        two: '#000000',
        three: '#a9adb1',
        grey1: '#cacccf',
        grey2: '#dbdde0',
        grey3: '#f6f6f6',
        grey4: '#a9adb1',
        grey5: '#000000',
        white: '#ffffff',
      },
      background: {
        default: '#f6f6f6',
        paper: '#fbfbfb',
        one: '#fbfbfb',
        two: '#ffffff',
        three: '#f5f5f5',
        four: '#f8f9fa',
        five: '#fbfcfd',
        six: '#ffffff',
      },
      text: {
        primary: 'rgba(0, 0, 0, 0.87)', // Material default
        secondary: '#000000',
        disabled: 'rgba(0, 0, 0, 0.7)', // Muted
      },
      success: {
        main: '#00bd8f',
        dark: '#4dffd4',
        light: '#00dba6',
      },
      warning: {
        main: '#dc9406',
        dark: '#ffc656',
        light: '#f6a609',
      },
      info: { // Same as secondary
        main: '#24b2ff',
        dark: '#21a1e7',
        light: '#79d0ff',
      },
      error: {
        main: '#e54e5c',
        dark: '#ff9fa8',
        light: '#ff5e6d',
      },
      graph: {
        ...COMMON_THEME.palette.graph,
        flamegraph: {
          kernel: '#90cb84',
          java: '#00b1d8',
          app: '#00b1d8',
          k8s: '#4796c1',
        },
      },
    },
    components: {
      ...COMMON_THEME.components,
      MuiDivider: {
        styleOverrides: {
          root: {
            backgroundColor: '#a9adb1', // background three.
          },
        },
      },
    },
  },
};

export const LIGHT_THEME = createTheme({
  ...LIGHT_BASE,
  palette: addSyntaxToPalette(LIGHT_BASE.palette),
});
