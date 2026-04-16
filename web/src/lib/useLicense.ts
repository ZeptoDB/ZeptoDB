"use client";
import { useState, useEffect } from "react";
import { fetchLicense, LicenseInfo } from "./api";

export function useLicense() {
  const [license, setLicense] = useState<LicenseInfo | null>(null);
  useEffect(() => { fetchLicense().then(setLicense); }, []);
  return license;
}

export function hasFeature(license: LicenseInfo | null, feature: string): boolean {
  if (!license) return false;
  return license.features.includes(feature);
}
