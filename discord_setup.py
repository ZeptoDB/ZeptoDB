import discord, asyncio

TOKEN = open("bottoken").read().strip()
GUILD_ID = 1492174712359354590

intents = discord.Intents.default()
client = discord.Client(intents=intents)

@client.event
async def on_ready():
    guild = client.get_guild(GUILD_ID)
    if not guild:
        print(f"ERROR: Guild {GUILD_ID} not found"); await client.close(); return

    print(f"Connected to: {guild.name}")

    # Create @Team role
    existing = discord.utils.get(guild.roles, name="Team")
    if not existing:
        await guild.create_role(name="Team", color=discord.Color.blue(), permissions=discord.Permissions(administrator=True))
        print("Created role: @Team")
    else:
        print("Role @Team already exists")

    # Create channels
    channels = {
        "announcements": "Release notes and updates",
        "general": "General discussion",
        "help": "Questions and support",
        "bug-reports": "Bug reporting",
        "github-feed": "GitHub webhook notifications",
    }
    for name, topic in channels.items():
        if not discord.utils.get(guild.text_channels, name=name):
            await guild.create_text_channel(name=name, topic=topic)
            print(f"Created #{name}")
        else:
            print(f"#{name} already exists")

    # Send welcome message to #general
    general = discord.utils.get(guild.text_channels, name="general")
    if general:
        await general.send(
            "**Welcome to ZeptoDB!** 🚀\n\n"
            "• Docs: https://zeptodb.com\n"
            "• GitHub: https://github.com/zeptodb/zeptodb\n\n"
            "Channels:\n"
            "• #announcements — release notes\n"
            "• #help — questions & support\n"
            "• #bug-reports — report issues\n"
            "• #github-feed — repo activity"
        )
        print("Sent welcome message to #general")

    print("Done!"); await client.close()

client.run(TOKEN)
