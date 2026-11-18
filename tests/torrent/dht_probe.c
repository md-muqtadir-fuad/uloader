/* DHT-only probe: runs the torrent.c DHT code for N seconds for an
   info-hash and prints when routers resolve and peers arrive. No TCP. */
#include "torrent.c"

int main(int argc, char **argv)
{
    const char *hex = argc > 1 ? argv[1] : "7acf8fb590b2060dd9c3146ef770169d593433b0";
    int secs = argc > 2 ? atoi(argv[2]) : 40;
    net_init();
    ul_config cfg;
    cfg_defaults(&cfg);
    ul_task *t = task_new("");
    bt *b = xcalloc(1, sizeof *b);
    b->t = t;
    b->cfg = &cfg;
    b->now = b->t0 = now_sec();
    rand_bytes(&b->rng, sizeof b->rng);
    b->c = xcalloc(MAX_CANDS, sizeof(cand));
    parse_btih(hex, strlen(hex), b->ih);
    dht_open(b);
    int last_nc = -1, last_routers = -1, last_nn = -1;
    while (b->now - b->t0 < secs) {
        b->now = now_sec();
        jobs_poll(b);
        dht_tick(b);
        net_poll(b, 50);
        if (b->nc != last_nc || b->d.nrouters != last_routers || (b->d.nn / 20) != last_nn) {
            printf("%6.2fs routers=%d nodes=%d pend=%d queries=%d searching=%d rounds=%d peers=%d\n", b->now - b->t0,
                   b->d.nrouters, b->d.nn, b->d.npend, b->d.queries, b->d.searching, b->d.rounds, b->nc);
            last_nc = b->nc;
            last_routers = b->d.nrouters;
            last_nn = b->d.nn / 20;
        }
    }
    printf("done: %d peers, %d rounds\n", b->nc, b->d.rounds);
    return 0;
}
