import TenantDetailPage from "./client";

export function generateStaticParams() {
  return [{ id: "_" }];
}

export default function Page() {
  return <TenantDetailPage />;
}
