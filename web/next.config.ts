import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  reactCompiler: true,
  async rewrites() {
    return [
      { source: "/api", destination: "http://localhost:8123/" },
      { source: "/api/", destination: "http://localhost:8123/" },
      { source: "/api/:path*", destination: "http://localhost:8123/:path*" },
    ];
  },
};

export default nextConfig;
