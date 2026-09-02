export interface NormalizedRange {
  magnitude: number;
  minUnit: number;
  spanUnit: number;
}

export function normalizedRange(min: number, max: number): NormalizedRange {
  const magnitude = Math.max(Math.abs(min), Math.abs(max), 1);
  const minUnit = min / magnitude;
  return {
    magnitude,
    minUnit,
    spanUnit: max / magnitude - minUnit || 1,
  };
}

export function normalizedPosition(value: number, range: NormalizedRange): number {
  return (value / range.magnitude - range.minUnit) / range.spanUnit;
}

export function histogramCountScale(bins: readonly { c: number }[]): number {
  let maxCount = 0;
  for (const bin of bins) maxCount = Math.max(maxCount, bin.c);
  return maxCount > 0 ? maxCount : 1;
}
