// server-side ai manager
namespace aiman
{
    int oldbotskillmin = -1, oldbotskillmax = -1, oldcoopskillmin = -1, oldcoopskillmax = -1, oldenemyskillmin = -1, oldenemyskillmax = -1,
        oldbotbalance = -2, oldbotlimit = -1, oldbotoffset = 0;
    float oldcoopbalance = -1, oldcoopmultibalance = -1;

    clientinfo *findaiclient(clientinfo *exclude = NULL)
    {
        clientinfo *least = NULL;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->clientnum < 0 || ci->state.aitype > AI_NONE || !ci->ready || ci == exclude) continue;
            if(!least || ci->bots.length() < least->bots.length()) least = ci;
        }
        return least;
    }

    void getskillrange(int type, int &m, int &n)
    {
        switch(type)
        {
            case AI_BOT:
                if(m_coop(gamemode, mutators))
                {
                    m = max(GAME(coopskillmax), GAME(coopskillmin));
                    n = min(GAME(coopskillmin), m);
                }
                else
                {
                    m = max(GAME(botskillmax), GAME(botskillmin));
                    n = min(GAME(botskillmin), m);
                }
                break;
            default:
                m = max(GAME(enemyskillmax), GAME(enemyskillmin));
                n = min(GAME(enemyskillmin), m);
                break;
        }
    }

    bool addai(int type, int ent, int skill)
    {
        int numbots = 0;
        loopv(clients)
        {
            if(type == AI_BOT && numbots >= GAME(botlimit)) return false;
            if(clients[i]->state.aitype == type)
            {
                clientinfo *ci = clients[i];
                if(ci->state.ownernum < 0)
                { // reuse a slot that was going to removed
                    clientinfo *owner = findaiclient();
                    if(!owner) return false;
                    ci->state.ownernum = owner->clientnum;
                    owner->bots.add(ci);
                    ci->state.aireinit = 1;
                    ci->state.aitype = type;
                    ci->state.aientity = ent;
                    return true;
                }
                if(type == AI_BOT) numbots++;
            }
        }
        if(type == AI_BOT && numbots >= GAME(botlimit)) return false;
        int cn = addclient(ST_REMOTE);
        if(cn >= 0)
        {
            clientinfo *ci = (clientinfo *)getinfo(cn);
            if(ci)
            {
                int s = skill, m = 100, n = 1;
                getskillrange(type, m, n);
                if(skill > m || skill < n) s = (m != n ? rnd(m-n) + n + 1 : m);
                ci->clientnum = cn;
                clientinfo *owner = findaiclient();
                ci->state.ownernum = owner ? owner->clientnum : -1;
                if(owner) owner->bots.add(ci);
                ci->state.aireinit = 2;
                ci->state.aitype = type;
                ci->state.aientity = ent;
                ci->state.skill = clamp(s, 1, 101);
                clients.add(ci);
                ci->state.lasttimeplayed = lastmillis;
                ci->state.colour = rnd(0xFFFFFF);
                ci->state.model = rnd(INT_MAX-1);
                copystring(ci->name, aistyle[ci->state.aitype].name, MAXNAMELEN);
                ci->state.state = CS_DEAD;
                ci->team = type == AI_BOT ? TEAM_NEUTRAL : TEAM_ENEMY;
                ci->online = ci->connected = ci->ready = true;
                return true;
            }
            delclient(cn);
        }
        return false;
    }

    void deleteai(clientinfo *ci)
    {
        if(ci->state.aitype == AI_NONE) return;
        int cn = ci->clientnum;
        loopv(clients) if(clients[i] != ci)
        {
            loopvk(clients[i]->state.fraglog) if(clients[i]->state.fraglog[k] == ci->clientnum)
                clients[i]->state.fraglog.remove(k--);
        }
        if(smode) smode->leavegame(ci, true);
        mutate(smuts, mut->leavegame(ci, true));
        ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
        savescore(ci);
        sendf(-1, 1, "ri3", N_DISCONNECT, cn, DISC_NONE);
        clientinfo *owner = (clientinfo *)getinfo(ci->state.ownernum);
        if(owner) owner->bots.removeobj(ci);
        clients.removeobj(ci);
        delclient(cn);
        dorefresh = 1;
    }

    bool delai(int type, bool skip)
    {
        bool retry = false;
        loopvrev(clients) if(clients[i]->state.aitype == type && clients[i]->state.ownernum >= 0)
        {
            if(!skip || clients[i]->state.state == CS_DEAD || clients[i]->state.state == CS_WAITING)
            {
                deleteai(clients[i]);
                return true;
            }
            else if(skip && !retry) retry = true;
        }
        if(skip && retry) delai(type, false);
        return false;
    }

    void reinitai(clientinfo *ci)
    {
        if(ci->state.aitype == AI_NONE) return;
        if(ci->state.ownernum < 0) deleteai(ci);
        else if(ci->state.aireinit >= 1)
        {
            if(ci->state.aireinit == 2) loopk(WEAP_MAX) loopj(2) ci->state.weapshots[k][j].reset();
            sendf(-1, 1, "ri6si3", N_INITAI, ci->clientnum, ci->state.ownernum, ci->state.aitype, ci->state.aientity, ci->state.skill, ci->name, ci->team, ci->state.colour, ci->state.model);
            if(ci->state.aireinit == 2)
            {
                waiting(ci, 1, DROP_RESET);
                if(smode) smode->entergame(ci);
                mutate(smuts, mut->entergame(ci));
            }
            ci->state.aireinit = 0;
        }
    }

    void shiftai(clientinfo *ci, clientinfo *owner = NULL)
    {
        clientinfo *prevowner = (clientinfo *)getinfo(ci->state.ownernum);
        if(prevowner) prevowner->bots.removeobj(ci);
        if(!owner) { ci->state.aireinit = 0; ci->state.ownernum = -1; }
        else if(ci->state.ownernum != owner->clientnum) { ci->state.aireinit = 1; ci->state.ownernum = owner->clientnum; owner->bots.add(ci); }
    }

    void removeai(clientinfo *ci, bool complete)
    { // either schedules a removal, or someone else to assign to
        loopvrev(ci->bots) shiftai(ci->bots[i], complete ? NULL : findaiclient(ci));
    }

    bool reassignai(clientinfo *exclude)
    {
        clientinfo *hi = NULL, *lo = NULL;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->clientnum < 0 || ci->state.aitype > AI_NONE || !ci->ready || ci == exclude)
                continue;
            if(!lo || ci->bots.length() < lo->bots.length()) lo = ci;
            if(!hi || hi->bots.length() > hi->bots.length()) hi = ci;
        }
        if(hi && lo && hi->bots.length() - lo->bots.length() > 1)
        {
            loopvrev(hi->bots)
            {
                shiftai(hi->bots[i], lo);
                return true;
            }
        }
        return false;
    }

    void checksetup()
    {
        int m = 100, n = 1, numbots = 0;
        loopv(clients) if(clients[i]->state.aitype > AI_NONE && clients[i]->state.ownernum >= 0)
        {
            clientinfo *ci = clients[i];
            getskillrange(clients[i]->state.aitype, m, n);
            if(ci->state.skill > m || ci->state.skill < n)
            { // needs re-skilling
                ci->state.skill = (m != n ? rnd(m-n) + n + 1 : m);
                if(!ci->state.aireinit) ci->state.aireinit = 1;
            }
            if(ci->state.aitype == AI_BOT && ++numbots >= GAME(botlimit)) shiftai(ci, NULL);
        }

        int balance = 0, people = numclients(-1, true, -1), numt = numteams(gamemode, mutators);
#ifdef MEKARCADE
        if(m_campaign(gamemode)) balance = GAME(campaignplayers); // campaigns strictly obeys nplayers
        else
#endif
        if(m_coop(gamemode, mutators))
        {
            numt--; // filter out the human team
            balance = people+int(ceilf(people*numt*(m_multi(gamemode, mutators) ? GAME(coopmultibalance) : GAME(coopbalance))));
        }
        else if(m_fight(gamemode) && !m_trial(gamemode) && GAME(botlimit) > 0)
        {
            switch(GAME(botbalance))
            {
                case -1: balance = max(people, m_duel(gamemode, mutators) ? 2 : nplayers); break; // use distributed numplayers
                case  0: balance = 0; break; // no bots
                default: balance = max(people, m_duel(gamemode, mutators) ? 2 : GAME(botbalance)); break; // balance to at least this
            }
            if(m_isteam(gamemode, mutators) && balance > 0)
            { // skew this if teams are unbalanced
                int plrs[TEAM_TOTAL] = {0}, highest = -1; // we do this because humans can unbalance in odd ways
                loopv(clients) if(clients[i]->state.aitype == AI_NONE && clients[i]->team >= TEAM_FIRST && isteam(gamemode, mutators, clients[i]->team, TEAM_FIRST))
                {
                    int team = clients[i]->team-TEAM_FIRST;
                    plrs[team]++;
                    if(highest < 0 || plrs[team] > plrs[highest]) highest = team;
                }
                if(highest >= 0)
                {
                    int bots = balance-people;
                    loopi(numt) if(i != highest && plrs[i] < plrs[highest]) loopj(plrs[highest]-plrs[i])
                    {
                        if(bots > 0) bots--;
                        else balance++;
                    }
                }
            }
        }
        balance += GAME(botoffset)*numt;
        int bots = balance-people;
        if(bots > GAME(botlimit)) balance -= bots-GAME(botlimit);
        if(balance > 0)
        {
            while(numclients(-1, true, AI_BOT) < balance) if(!addai(AI_BOT)) break;
            while(numclients(-1, true, AI_BOT) > balance) if(!delai(AI_BOT)) break;
            if(m_isteam(gamemode, mutators)) loopvrev(clients)
            {
                clientinfo *ci = clients[i];
                if(ci->state.aitype == AI_BOT && ci->state.ownernum >= 0)
                {
                    int teamb = chooseteam(ci, ci->team);
                    if(ci->team != teamb) setteam(ci, teamb, true, true);
                }
            }
        }
        else clearai(1);
    }

    const float MAXSPAWNDIST = 512.f;
    const float MINSPAWNDIST = 64.f;
    void checkenemies()
    {
        if(m_enemies(gamemode, mutators))
        {
            loopvj(sents) if(sents[j].type == ACTOR && sents[j].attrs[0] >= 0 && sents[j].attrs[0] < AI_TOTAL && gamemillis >= sents[j].millis && (sents[j].attrs[5] == triggerid || !sents[j].attrs[5]) && m_check(sents[j].attrs[3], sents[j].attrs[4], gamemode, mutators))
            {
#ifdef MEKARCADE
                bool allow = !m_campaign(gamemode);
                if(!allow)
                {
                    loopv(clients) if(clients[i]->state.aitype < AI_START)
                    {
                        float dist = clients[i]->state.o.dist(sents[j].o);
                        if(dist <= MAXSPAWNDIST) allow = true;
                        else if(allow && dist <= MINSPAWNDIST)
                        {
                            allow = false;
                            break;
                        }
                    }
                }
#else
                bool allow = true;
#endif
                int count = 0;
                loopvrev(clients) if(clients[i]->state.aientity == j)
                {
                    count++;
                    if(!allow || count > GAME(enemybalance)) deleteai(clients[i]);
                }
                if(allow && count < GAME(enemybalance))
                {
                    int amt = GAME(enemybalance)-count;
                    loopk(amt) addai(sents[j].attrs[0]+AI_START, j);
                }
            }
        }
        else clearai(2);
    }

    void clearai(int type)
    { // clear and remove all ai immediately
        loopvrev(clients) if(!type || (type == 2 ? clients[i]->state.aitype >= AI_START : clients[i]->state.aitype == AI_BOT))
            deleteai(clients[i]);
    }

    void checkai()
    {
        if(!m_demo(gamemode) && numclients())
        {
            if(hasgameinfo && !interm)
            {
                if(!dorefresh)
                {
                    #define checkold(n) if(old##n != GAME(n)) { dorefresh = 1; old##n = GAME(n); }
                    if(m_enemies(gamemode, mutators))
                    {
                        checkold(enemyskillmin);
                        checkold(enemyskillmax);
                    }
                    if(m_coop(gamemode, mutators))
                    {
                        checkold(coopskillmin);
                        checkold(coopskillmax);
                        if(m_multi(gamemode, mutators)) { checkold(coopmultibalance); }
                        else { checkold(coopbalance); }
                    }
                    else
                    {
                        checkold(botskillmin);
                        checkold(botskillmax);
                        checkold(botbalance);
                    }
                    checkold(botlimit);
                    checkold(botoffset);
                }
                if(dorefresh)
                {
                    dorefresh -= curtime;
                    if(dorefresh <= 0) { dorefresh = 0; checksetup(); }
                }
                checkenemies();
                loopvrev(clients) if(clients[i]->state.aitype > AI_NONE) reinitai(clients[i]);
                while(true) if(!reassignai()) break;
            }
        }
        else clearai();
    }
}
