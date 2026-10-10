# MedEvac — simple UI + deployment starter

This is a student-project demo. It serves the pages and API from the **same C++ server**, so the browser can call `/api/...` without setting up a separate frontend URL or CORS configuration.

## Run locally on macOS

1. Install SQLite development files if needed:

   ```bash
   brew install sqlite
   ```

2. Open Terminal in this folder and compile:

   ```bash
   clang++ -std=c++17 -pthread server.cpp -lsqlite3 -o medevac_server
   ```

3. Set your admin key and run:

   ```bash
   export MEDEVAC_ADMIN_KEY='choose-a-long-secret'
   ./medevac_server
   ```

4. Open:
   - `http://localhost:18080/`
   - `http://localhost:18080/register.html`
   - `http://localhost:18080/admin.html`

Keep the Terminal running while you use the pages.

## Easiest online demo: Render (one service)

The included `Dockerfile` builds the C++ server and serves the simple HTML pages from the same service. `render.yaml` is a starter blueprint.

1. Put this folder in a GitHub repository.
2. In Render, choose **New → Blueprint** and connect the repository.
3. Set `MEDEVAC_ADMIN_KEY` to a long random secret when asked.
4. Deploy and open the Render URL.

**Important:** the Render disk in `render.yaml` is persistent, but persistent disks may require a paid plan and availability/pricing can change. If Render will not allow the selected plan to use a disk, use a plan with persistent disk support or switch the database to managed PostgreSQL before treating data as persistent. Without persistent storage, SQLite data can be lost on redeploy/restart.

The C++ server now reads the platform `PORT` environment variable automatically. If it is not set, it uses port 18080 locally.

## Can I use Vercel?

Vercel is best for the frontend; it does not run this always-on C++ socket server as a normal web service. The simplest setup is to host this entire demo on one container-capable service such as Render. If you specifically want Vercel, deploy the frontend there and host the C++ API separately, then change frontend requests to use the API's public HTTPS URL and configure strict CORS. That is more steps, so I recommend one service first.

## Demo limitations

- Use fake/synthetic data only. This is not a real emergency response system.
- The prototype uses a small custom HTTP server and basic request parsing; do not expose it to real users or sensitive data.
- The admin key is a demo gate, not complete production authentication.
- Database persistence and backups must be verified after deployment.
- Organizations must use location names that exist in the demo road graph for routing.


## Easy admin access (student demo only)

Open `/admin.html`. The demo key is prefilled as `medevac-demo-admin-2026`.

For Render, set the service environment variable `MEDEVAC_ADMIN_KEY` to exactly that value and redeploy. The included `render.yaml` uses this value for a demo deployment. This shared key is not safe for a real/public emergency service; replace it with a private secret before public use.
