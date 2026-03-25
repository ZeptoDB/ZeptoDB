"use client";
import { useAuth } from "@/lib/auth";
import { useRouter } from "next/navigation";
import { useEffect, type ReactNode } from "react";

export default function AuthGuard({ children }: { children: ReactNode }) {
  const { auth } = useAuth();
  const router = useRouter();
  useEffect(() => { if (!auth) router.replace("/login"); }, [auth, router]);
  if (!auth) return null;
  return <>{children}</>;
}
