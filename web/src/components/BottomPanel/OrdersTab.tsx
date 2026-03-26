export function OrdersTab() {
  return (
    <div className="h-full flex flex-col">
      <table className="w-full text-xs">
        <thead className="bg-[#161a25] text-[#787b86]">
          <tr>
            <th className="text-left px-3 py-2 font-medium">ID</th>
            <th className="text-left px-3 py-2 font-medium">Time</th>
            <th className="text-left px-3 py-2 font-medium">Symbol</th>
            <th className="text-left px-3 py-2 font-medium">Side</th>
            <th className="text-left px-3 py-2 font-medium">Type</th>
            <th className="text-right px-3 py-2 font-medium">Qty</th>
            <th className="text-right px-3 py-2 font-medium">Price</th>
            <th className="text-right px-3 py-2 font-medium">Status</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td colSpan={8} className="text-center text-[#787b86] py-6 text-xs">
              Order state updates not yet available — requires engine-side implementation.
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  );
}
