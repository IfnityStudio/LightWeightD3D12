using System.Windows;

namespace WpfTriangle;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        ApplyClearColor();
        ApplyAnimationSpeed();
    }

    private void OnClearColorChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsLoaded)
        {
            return;
        }

        ApplyClearColor();
    }

    private void OnSpeedChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsLoaded)
        {
            return;
        }

        ApplyAnimationSpeed();
    }

    private void ApplyClearColor()
    {
        RenderHost.SetClearColor((float)RedSlider.Value, (float)GreenSlider.Value, (float)BlueSlider.Value);
    }

    private void ApplyAnimationSpeed()
    {
        RenderHost.SetAnimationSpeed((float)SpeedSlider.Value);
    }
}
