// The player's position when the character played is not Kliff.
//
// AsunaKaoru's bug on 2 November: no marker placed and nothing pinned, while
// playing Damiane. Her log had the player actor, the entity with the login,
// detect and special mode components, at the loading placeholder (0, 1000, 0)
// from the first read to the last, and the flash's tail naming B0100002 each
// time it went up. Damiane's body is a second entity, and Master Looter has
// found it by the type tag 04 and status category 0E it alone wears.
//
// These drive the real player.cpp with heap blocks standing in for the
// player actor, the body, and the world objects around them.
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

namespace
{
    int g_secondLines = 0, g_againLines = 0;
}
namespace gs::Log
{
    void Write(const char* level, const char* fmt, ...)
    {
        (void)level;
        if (!fmt) return;
        if (strstr(fmt, "a second character is being played")) ++g_secondLines;
        if (strstr(fmt, "again, so positions come from it")) ++g_againLines;
    }
}
namespace gs::typescan
{
    bool ModuleRange(uintptr_t& base, size_t& size) { base = 0; size = 0; return false; }
    int FindGlobals(uintptr_t, uintptr_t*, int) { return 0; }
}

// The flash's tail and the actor manager's pools are what the tests set.
uint32_t g_tail = 0;
uintptr_t g_pool[2048];
int g_poolN = 0, g_offered = 0;
namespace gs::aim
{
    void SetPlayerActor(uintptr_t) {}
    void SetDetectComponent(uintptr_t) {}
    void SetSpecialComponent(uintptr_t) {}
    bool ModeRecords(uint16_t* first, uint16_t* second, uint32_t* firstTail, uint32_t* secondTail)
    {
        *first = g_tail ? 7 : 0xFFFF;
        *second = 0xFFFF;
        *firstTail = g_tail;
        *secondTail = 0;
        return true;
    }
}
namespace gs::actors
{
    int Offered(uintptr_t* out, int n)
    {
        ++g_offered;
        const int k = g_poolN < n ? g_poolN : n;
        memcpy(out, g_pool, k * sizeof(uintptr_t));
        return k;
    }
}

namespace gs::load
{
    void CountQuery() {}
}
#include "game/rtti.cpp"
#include "game/player.cpp"

// The camera's world copy of the played body, which the tests set.
bool g_camValid = false;
float g_cam[3]{};
namespace gs::camera
{
    Pose Read()
    {
        Pose p;
        p.valid = true;
        p.worldValid = g_camValid;
        memcpy(p.world, g_cam, sizeof(g_cam));
        return p;
    }
}

// Real classes, so the RTTI the reader names components by is real too.
namespace pa
{
    struct ClientStatusActorComponent
    {
        virtual ~ClientStatusActorComponent() {}
        unsigned char bytes[0x300]{};
    };
    struct ClientChildOnlyInGameActor
    {
        virtual ~ClientChildOnlyInGameActor() {}
    };
}

namespace
{
    int failures = 0;

    void Expect(const char* what, bool ok)
    {
        printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

    // An entity with a component block and a transform, laid out at the
    // offsets the game uses.
    struct Ent
    {
        unsigned char* e;
        uintptr_t* block;
        unsigned char* tf;
        unsigned char typeInfo[8];
        pa::ClientStatusActorComponent status;

        Ent(uint32_t eid, uint8_t tag, uint8_t cat2)
        {
            e = static_cast<unsigned char*>(_aligned_malloc(0x100, 16));
            block = static_cast<uintptr_t*>(_aligned_malloc(0x400, 16));
            tf = static_cast<unsigned char*>(_aligned_malloc(0x600, 16));
            memset(e, 0, 0x100);
            memset(block, 0, 0x400);
            memset(tf, 0, 0x600);
            memset(typeInfo, 0, sizeof(typeInfo));
            typeInfo[1] = tag;
            status.bytes[0x2F0 - 8] = cat2;
            SetEid(eid);
            *reinterpret_cast<uintptr_t*>(e + 0x68) = reinterpret_cast<uintptr_t>(block);
            *reinterpret_cast<uintptr_t*>(e + 0x88) = reinterpret_cast<uintptr_t>(typeInfo);
            block[0x20 / 8] = reinterpret_cast<uintptr_t>(&status);
            block[0x1A0 / 8] = reinterpret_cast<uintptr_t>(tf);
            *reinterpret_cast<uint32_t*>(tf + 0xC8) = 0xFFFFFFFF;
        }
        void SetEid(uint32_t eid) { *reinterpret_cast<uint32_t*>(e + 0x60) = eid; }
        // World, and local as world minus a sub-level origin.
        void At(float x, float y, float z, float ox = -12000.0f, float oz = -6000.0f)
        {
            const float w[3] = {x, y, z}, l[3] = {x - ox, y, z - oz};
            memcpy(tf + 0x29C, w, 12);
            memcpy(tf + 0xB4, l, 12);
        }
        // World and local given apart, for a world position that is wrong.
        void Raw(float wx, float wy, float wz, float lx, float ly, float lz)
        {
            const float w[3] = {wx, wy, wz}, l[3] = {lx, ly, lz};
            memcpy(tf + 0x29C, w, 12);
            memcpy(tf + 0xB4, l, 12);
        }
        uintptr_t Ptr() const { return reinterpret_cast<uintptr_t>(e); }
    };

    // The player actor carries the special mode component, which knows the
    // actor at +0x08.
    unsigned char g_special[0x80];

    void Pool(std::initializer_list<const Ent*> bodies, const Ent* worldObjects, int nWorld)
    {
        g_poolN = 0;
        for (int i = 0; i < nWorld; ++i) g_pool[g_poolN++] = worldObjects[i].Ptr();
        for (const Ent* b : bodies) g_pool[g_poolN++] = b->Ptr();
    }
}

int main()
{
    // The player actor, the one Recover finds by its login component.
    static Ent actor(0xA0100001, 0x01, 0x00);
    static pa::ClientChildOnlyInGameActor actorClass;
    memcpy(actor.e, &actorClass, sizeof(void*));   // its vtable, so it names itself
    memset(g_special, 0, sizeof(g_special));
    *reinterpret_cast<uintptr_t*>(g_special + 0x08) = actor.Ptr();
    gs::player::SetSpecialComponent(g_special);

    // Fifteen hundred world objects, all B0 like the body, none of them it.
    static Ent* world = static_cast<Ent*>(operator new(sizeof(Ent) * 1500));
    for (int i = 0; i < 1500; ++i)
    {
        new (&world[i]) Ent(0xB0200000 + i, 0x03, 0x0A);
        world[i].At(-11900.0f + i, 900.0f, -5700.0f);
    }
    static Ent damiane(0xB0100002, 0x04, 0x0E);
    damiane.At(-11937.0f, 926.8f, -5766.3f);
    // A character standing nearby wearing the same tag and another category.
    static Ent bystander(0xB0100009, 0x04, 0x0A);
    bystander.At(-11950.0f, 926.0f, -5770.0f);
    Pool({&bystander, &damiane}, world, 1500);

    printf("-- Kliff, the player actor where he stands --\n");
    actor.At(-9000.0f, 50.0f, -4000.0f);
    gs::player::Pos p = gs::player::Read();
    Expect("the position is the player actor's", p.valid && Near(p.x, -9000.0f) && Near(p.z, -4000.0f));
    Expect("and the pools are not searched", g_offered == 0);

    printf("-- Kliff waiting out a load --\n");
    actor.At(0.0f, 1000.0f, 0.0f, 0.0f, 0.0f);
    Sleep(600);   // positions are chosen twice a second
    p = gs::player::Read();
    Expect("the placeholder stands, with nothing to say a second character is played", p.valid && Near(p.x, 0.0f) && Near(p.y, 1000.0f));
    Expect("and the pools are still not searched", g_offered == 0);

    printf("-- Damiane, the flash naming B0100002 --\n");
    g_tail = 0xB0100002;
    Sleep(600);   // positions are chosen twice a second
    p = gs::player::Read();
    Expect("the position is her body's world position", p.valid && Near(p.x, -11937.0f) && Near(p.y, 926.8f) && Near(p.z, -5766.3f));
    Expect("and the local one is her body's too", Near(p.lx, -11937.0f + 12000.0f) && Near(p.lz, -5766.3f + 6000.0f));
    Expect("so the sub-level origin is hers", Near(p.ox, -12000.0f) && Near(p.oz, -6000.0f));
    Expect("the actor is still the player actor", gs::player::Actor() == actor.Ptr());
    Expect("the change is logged once", g_secondLines == 1);
    const int searched = g_offered;
    Expect("one search found her", searched == 1);

    printf("-- the flash goes down and she walks --\n");
    g_tail = 0;
    damiane.At(-11930.0f, 927.0f, -5760.0f);
    p = gs::player::Read();
    Expect("the position follows her body", p.valid && Near(p.x, -11930.0f) && Near(p.z, -5760.0f));
    for (int i = 0; i < 50; ++i) gs::player::Read();
    Expect("fifty more reads search nothing", g_offered == searched);
    Expect("and log nothing more", g_secondLines == 1);

    printf("-- a load frees her body and builds it again elsewhere --\n");
    damiane.SetEid(0xB0999999);   // the block is somebody else's now
    damiane.At(5.0f, 0.0f, 5.0f, 0.0f, 0.0f);
    static Ent damiane2(0xB0100002, 0x04, 0x0E);
    damiane2.At(-3000.0f, 200.0f, 1500.0f, -3000.0f, 0.0f);
    Pool({&bystander, &damiane2}, world, 1500);
    Sleep(1100);
    p = gs::player::Read();
    Expect("the new body with her id is found without the flash", p.valid && Near(p.x, -3000.0f) && Near(p.z, 1500.0f));
    Expect("and the stale block is not read for her", !Near(p.x, 5.0f));

    printf("-- a different body with no flash to name it --\n");
    static Ent oongka(0xB0100007, 0x04, 0x0E);
    oongka.At(800.0f, 300.0f, 900.0f, 0.0f, 0.0f);
    damiane2.SetEid(0xB0888888);
    Pool({&bystander, &oongka}, world, 1500);
    Sleep(1100);
    p = gs::player::Read();
    Expect("the body wearing tag 04 and category 0E is taken", p.valid && Near(p.x, 800.0f) && Near(p.z, 900.0f));
    Expect("and not the bystander with the same tag", !Near(p.x, -11950.0f));
    Expect("the new body is logged, and the rebuilt one with her id was not", g_secondLines == 2);

    printf("-- swapped back to Kliff --\n");
    actor.At(-9100.0f, 52.0f, -4100.0f);
    Sleep(600);   // positions are chosen twice a second
    p = gs::player::Read();
    Expect("the position is the player actor's again", p.valid && Near(p.x, -9100.0f) && Near(p.z, -4100.0f));
    Expect("and the switch is logged", g_againLines == 1);

    printf("-- every body on the placeholder during a load --\n");
    actor.At(0.0f, 1000.0f, 0.0f, 0.0f, 0.0f);
    oongka.At(0.0f, 1000.0f, 0.0f, 0.0f, 0.0f);
    Sleep(1100);
    p = gs::player::Read();
    Expect("the placeholder stands rather than any body", p.valid && Near(p.x, 0.0f) && Near(p.z, 0.0f));

    // AsunaKaoru's 4 November log on 1.1.38. The actor is not on the
    // placeholder any more: it stays where Kliff last stood while Damiane
    // walks, and only the camera moves with her.
    printf("-- Damiane, the actor left standing where Kliff was, no flash yet --\n");
    g_tail = 0;
    actor.At(-12083.6f, 866.8f, -4884.1f, -12000.0f, -5000.0f);
    static Ent damiane3(0xB0100002, 0x04, 0x0E);
    damiane3.At(-11911.5f, 834.2f, -5022.4f, -11000.0f, -5000.0f);
    // Another body wearing her tags, far from the camera, which must lose.
    damiane2.At(-3000.0f, 200.0f, 1500.0f, -3000.0f, 0.0f);
    Pool({&bystander, &damiane2, &oongka, &damiane3}, world, 1500);
    g_camValid = true;
    g_cam[0] = -11911.5f; g_cam[1] = 835.7f; g_cam[2] = -5022.4f;
    Sleep(5100);
    p = gs::player::Read();
    Expect("the position is her body's, beside the camera", p.valid && Near(p.x, -11911.5f) && Near(p.z, -5022.4f));
    Expect("not the actor's 212 m away", !Near(p.x, -12083.6f));
    Expect("and not the other body with her tags", !Near(p.x, -3000.0f));
    Expect("so the sub-level origin is hers", Near(p.ox, -11000.0f) && Near(p.oz, -5000.0f));

    printf("-- she walks with the flash down --\n");
    damiane3.At(-11905.0f, 834.0f, -5015.0f, -11000.0f, -5000.0f);
    g_cam[0] = -11905.0f; g_cam[1] = 835.5f; g_cam[2] = -5015.0f;
    Sleep(600);
    p = gs::player::Read();
    Expect("the position follows her body", p.valid && Near(p.x, -11905.0f) && Near(p.z, -5015.0f));

    printf("-- Kliff after a load: the world position reads near zero and the local one is right --\n");
    g_tail = 0xA0100001;
    actor.Raw(0.44f, 0.566f, 0.847f, -918.162f, 834.105f, -18.530f);
    g_cam[0] = -11917.8f; g_cam[1] = 835.6f; g_cam[2] = -5019.5f;
    Sleep(600);
    p = gs::player::Read();
    Expect("the position is his local one in the camera's origin",
           p.valid && Near(p.x, -11918.162f) && Near(p.y, 834.105f) && Near(p.z, -5018.530f));
    Expect("so the origin is (-11000, 0, -5000)", Near(p.ox, -11000.0f) && Near(p.oy, 0.0f) && Near(p.oz, -5000.0f));
    Expect("and the flash naming him dropped Damiane's body", gs::player::Actor() == actor.Ptr() && g_bodyEid.load() == 0);
    for (int i = 0; i < 20; ++i) p = gs::player::Read();
    Expect("reads between choices keep the origin", p.valid && Near(p.x, -11918.162f) && Near(p.z, -5018.530f));
    actor.At(0.0f, 1000.0f, 0.0f, 0.0f, 0.0f);
    p = gs::player::Read();
    Expect("a load in between reads the placeholder, not the placeholder moved by his origin",
           p.valid && Near(p.x, 0.0f) && Near(p.z, 0.0f));
    actor.Raw(0.44f, 0.566f, 0.847f, -918.162f, 834.105f, -18.530f);

    printf("-- his world position comes good --\n");
    actor.At(-11906.8f, 832.6f, -5032.7f, -11000.0f, -5000.0f);
    g_cam[0] = -11906.8f; g_cam[1] = 834.1f; g_cam[2] = -5032.7f;
    Sleep(600);
    p = gs::player::Read();
    Expect("the position is the actor's own", p.valid && Near(p.x, -11906.8f) && Near(p.z, -5032.7f));

    printf("-- back to Kliff with the flash's tail still naming Damiane --\n");
    g_tail = 0xB0100002;
    Sleep(1100);
    p = gs::player::Read();
    Expect("the camera agrees with the actor, so the actor wins", p.valid && Near(p.x, -11906.8f) && Near(p.z, -5032.7f));

    printf("-- no camera, the flash naming Damiane, the actor off the placeholder --\n");
    g_camValid = false;
    actor.At(-12083.6f, 866.8f, -4884.1f, -12000.0f, -5000.0f);
    Sleep(1100);
    p = gs::player::Read();
    Expect("the flash's word picks her body", p.valid && Near(p.x, -11905.0f) && Near(p.z, -5015.0f));

    printf("-- no camera, Kliff walking with no flash --\n");
    g_tail = 0xA0100001;
    actor.At(-9200.0f, 52.0f, -4200.0f);
    Sleep(600);
    p = gs::player::Read();
    Expect("the actor, as before", p.valid && Near(p.x, -9200.0f) && Near(p.z, -4200.0f));

    printf("\n%s\n", failures ? "FAILURES" : "all pass");
    return failures ? 1 : 0;
}
