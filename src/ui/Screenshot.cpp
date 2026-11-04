#include "ui/Screenshot.h"
#include "ui/MainComponent.h"
#include "ui/LevelMeter.h"
#include <juce_graphics/juce_graphics.h>

namespace ensayo
{
namespace
{
    constexpr int shotWidth = 1100, shotHeight = 760;

    juce::String typeName (const juce::Component& c)
    {
        if (dynamic_cast<const juce::TextEditor*> (&c) != nullptr)   return "TextEditor";
        if (dynamic_cast<const juce::ComboBox*> (&c) != nullptr)     return "ComboBox";
        if (dynamic_cast<const juce::TextButton*> (&c) != nullptr)   return "TextButton";
        if (dynamic_cast<const juce::ToggleButton*> (&c) != nullptr) return "ToggleButton";
        if (dynamic_cast<const juce::Label*> (&c) != nullptr)        return "Label";
        if (dynamic_cast<const juce::Slider*> (&c) != nullptr)       return "Slider";
        if (dynamic_cast<const juce::ProgressBar*> (&c) != nullptr)  return "ProgressBar";
        if (dynamic_cast<const LevelMeter*> (&c) != nullptr)         return "LevelMeter";
        if (dynamic_cast<const juce::Viewport*> (&c) != nullptr)     return "Viewport";
        return "Component";
    }

    // isShowing() needs a peer; for a detached tree "visible" means every component up to the root is visible.
    bool isVisibleWithin (const juce::Component& root, const juce::Component& c)
    {
        for (auto* p = &c; p != nullptr; p = p->getParentComponent())
        {
            if (! p->isVisible())
                return false;

            if (p == &root)
                return true;
        }

        return false;
    }

    void collectControls (juce::Component& root, juce::Component& c, const juce::String& view, juce::Array<juce::var>& out)
    {
        if (&c != &root && c.getName().isNotEmpty())
        {
            const auto area = root.getLocalArea (&c, c.getLocalBounds());
            auto* object = new juce::DynamicObject();
            object->setProperty ("name", c.getName());
            object->setProperty ("type", typeName (c));
            object->setProperty ("x", area.getX());
            object->setProperty ("y", area.getY());
            object->setProperty ("w", area.getWidth());
            object->setProperty ("h", area.getHeight());
            object->setProperty ("visible", isVisibleWithin (root, c));
            object->setProperty ("view", view);
            out.add (juce::var (object));
        }

        for (auto* child : c.getChildren())
            collectControls (root, *child, view, out);
    }

    juce::Result writePng (juce::Component& root, const juce::File& file)
    {
        juce::Image image (juce::SoftwareImageType().create (juce::Image::ARGB, root.getWidth(), root.getHeight(), true));

        {
            juce::Graphics g (image);
            root.paintEntireComponent (g, true);
        }

        file.deleteFile();
        juce::FileOutputStream stream (file);

        if (stream.failedToOpen())
            return juce::Result::fail ("No se pudo escribir " + file.getFullPathName());

        if (! juce::PNGImageFormat().writeImageToStream (image, stream))
            return juce::Result::fail ("No se pudo codificar " + file.getFullPathName());

        return juce::Result::ok();
    }
}

juce::Result runScreenshots (const juce::File& outDir)
{
    if (const auto created = outDir.createDirectory(); created.failed())
        return created;

    const bool previousTestMode = MainComponent::isTestMode();
    MainComponent::setTestMode (true);

    juce::Array<juce::var> controls;
    juce::Result result = juce::Result::ok();

    {
        MainComponent main;
        main.setVisible (true);
        main.setSize (shotWidth, shotHeight);

        result = writePng (main, outDir.getChildFile ("lobby.png"));
        collectControls (main, main, "lobby", controls);

        if (result.wasOk())
        {
            main.showDemoSession();
            main.resized();
            result = writePng (main, outDir.getChildFile ("session.png"));
            collectControls (main, main, "session", controls);
        }
    }

    MainComponent::setTestMode (previousTestMode);

    if (result.failed())
        return result;

    const auto jsonFile = outDir.getChildFile ("controls.json");

    if (! jsonFile.replaceWithText (juce::JSON::toString (juce::var (controls), false), false, false, "\n"))
        return juce::Result::fail ("No se pudo escribir " + jsonFile.getFullPathName());

    return juce::Result::ok();
}
}
