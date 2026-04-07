import TableDetailPage from "./client";

export function generateStaticParams() {
  return [{ name: "_" }];
}

export default function Page() {
  return <TableDetailPage />;
}
