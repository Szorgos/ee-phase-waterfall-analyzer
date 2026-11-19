import { applyD1Migrations } from "cloudflare:test";
import { env } from "cloudflare:workers";

declare global {
  namespace Cloudflare {
    interface Env {
      DB: D1Database;
      RATE_SALT: string;
      TEST_MIGRATIONS: import("cloudflare:test").D1Migration[];
    }
    interface GlobalProps {
      mainModule: typeof import("../src/index");
    }
  }
}

await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);
