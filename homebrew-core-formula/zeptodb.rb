class Zeptodb < Formula
  desc "Ultra-low latency in-memory time-series database"
  homepage "https://zeptodb.com"
  url "https://github.com/ZeptoDB/ZeptoDB/archive/refs/tags/v0.0.3.tar.gz"
  sha256 "a6e3c0184037163335c6a07b137e0efcb794ec93a6d2e2435ea5197b13be0829"
  license "BSL-1.1"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "llvm@19"
  depends_on "highway"
  depends_on "openssl@3"
  depends_on "lz4"
  depends_on "readline"

  def install
    llvm = Formula["llvm@19"]

    args = %W[
      -GNinja
      -DCMAKE_C_COMPILER=#{llvm.opt_bin}/clang
      -DCMAKE_CXX_COMPILER=#{llvm.opt_bin}/clang++
      -DZEPTO_BUILD_TESTS=OFF
      -DZEPTO_BUILD_BENCH=OFF
      -DZEPTO_USE_S3=OFF
      -DZEPTO_USE_KAFKA=OFF
      -DZEPTO_USE_FLIGHT=OFF
      -DZEPTO_USE_PARQUET=OFF
      -DZEPTO_USE_UCX=OFF
      -DZEPTO_USE_HUGEPAGES=OFF
      -DZEPTO_USE_TCMALLOC=OFF
      -DZEPTO_USE_LTO=OFF
    ]

    system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
    system "ninja", "-C", "build", "zepto-cli", "zepto_http_server", "zepto_data_node"

    bin.install "build/tools/zepto-cli" => "zeptodb"
    bin.install "build/tools/zepto_http_server"
    bin.install "build/tools/zepto_data_node"
  end

  test do
    assert_match "ZeptoDB", shell_output("#{bin}/zeptodb --version 2>&1", 0)
  end
end
