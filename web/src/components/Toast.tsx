import { createContext, useContext, useState, useCallback, type ReactNode } from 'react';
import { cn } from '../utils/format';

interface Toast {
  id: number;
  message: string;
  type: 'info' | 'success' | 'error';
}

interface ToastContextValue {
  toast: (message: string, type?: Toast['type']) => void;
}

const ToastContext = createContext<ToastContextValue>({ toast: () => {} });

let nextId = 0;

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);

  const addToast = useCallback((message: string, type: Toast['type'] = 'info') => {
    const id = ++nextId;
    setToasts((prev) => [...prev, { id, message, type }]);
    setTimeout(() => {
      setToasts((prev) => prev.filter((t) => t.id !== id));
    }, 3000);
  }, []);

  return (
    <ToastContext.Provider value={{ toast: addToast }}>
      {children}
      {/* Toast container */}
      <div className="fixed top-14 right-4 z-50 flex flex-col gap-2 pointer-events-none">
        {toasts.map((t) => (
          <div
            key={t.id}
            className={cn(
              'px-4 py-2 rounded-lg text-xs font-medium shadow-lg animate-[fadeIn_0.2s_ease-out] pointer-events-auto',
              t.type === 'success' && 'bg-[#26a69a] text-white',
              t.type === 'error' && 'bg-[#ef5350] text-white',
              t.type === 'info' && 'bg-[#2962ff] text-white',
            )}
          >
            {t.message}
          </div>
        ))}
      </div>
    </ToastContext.Provider>
  );
}

export const useToast = () => useContext(ToastContext);
