import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  output: "export",
  basePath: "/ui",
  reactCompiler: true,
  images: { unoptimized: true },
};

export default nextConfig;
