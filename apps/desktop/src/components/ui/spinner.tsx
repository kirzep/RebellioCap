import { t, useTranslation } from '../../i18n';
import { cn } from "cn"
import { Loader2Icon } from "lucide-react"

function Spinner({ className, ...props }: React.ComponentProps<"svg">) {
  useTranslation();
  return (
    <Loader2Icon
      role="status"
      aria-label={t("Загрузка")}
      className={cn("size-4 animate-spin", className)}
      {...props}
    />
  )
}

export { Spinner }
