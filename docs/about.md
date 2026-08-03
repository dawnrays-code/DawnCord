# The story behind DawnCord

This is yet another client for Discord for the PS Vita. It takes
inspiration from [VitaCord](https://github.com/devingDev/VitaCord), the
original port from many extremely talented and respected individuals in
the community, which unfortunately dates back to when Discord had just
come out (2016 era), and for that reason is missing many features that
were introduced later on.

A personal note: while I've been working in IT for a decade as a sysadmin,
unfortunately I don't have much experience in coding. For that reason,
this project was primarily vibe-coded with Claude (Fable 5), and I can't
guarantee the solid codebase you'd expect from a senior
engineer/developer. Also, it cannot replicate the joy of figuring out why
the code wouldn't run or compile at 4 AM. It's something you only get by
spending time and effort on your creation, a personal joy that AI coding
does not convey. Well, I still have extensive knowledge in clients,
networks and what can feasibly be achieved by such tools, which helped me
direct the workflow and the specifics. But yeah, the point above still
stands.

Anyway, technical and existential AI considerations aside, it came out to
be a fun and decently working project, which can bring a cool app we all
use every day to the PS Vita, and for that I'm really thankful.

Also, I got my hands on a Vita for the first time only around 2 months
ago, since I could not really afford it when I was younger. And, thanks to
incredible tools like Moonlight-vita by xyzz, it's pretty much a
PlayStation Portal with an OLED screen. Porting over Discord only seemed
right.

I hope you can try it out and let me know if it works.

## Credits

Built by dawnrays and Claude (Anthropic's Fable 5). Third-party code used:

- [VitaSDK](https://vitasdk.org) and
  [vita2d](https://github.com/xerpi/libvita2d), everything the Vita draws
- [cJSON](https://github.com/DaveGamble/cJSON), vendored, MIT
- [discord.py-self](https://github.com/dolfies/discord.py-self), the
  Discord side of the companion
- [davey](https://github.com/Snazzah/davey), the DAVE and MLS crypto
- [Playwright](https://playwright.dev), optional, used only for the
  browser login

Thanks to devingDev for the original
[VitaCord](https://github.com/devingDev/VitaCord), proving there's an
active interest in the community.
