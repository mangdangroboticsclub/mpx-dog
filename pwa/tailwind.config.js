/** @type {import('tailwindcss').Config} */
export default {
  content: ["./src/**/*.{html,svelte,js,ts}"],
  theme: {
    extend: {
      colors: {
        mpx: {
          orange: "#d35400",
          "orange-light": "#e67e22",
          bg: "#1e1e2e",
          surface: "#2d2d44",
          text: "#e0e0e0",
          muted: "#8888aa",
        },
      },
      fontFamily: {
        mono: ["'JetBrains Mono'", "'Fira Code'", "monospace"],
      },
    },
  },
  plugins: [
    require("@tailwindcss/typography"),
  ],
};
